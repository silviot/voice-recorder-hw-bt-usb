/* Host binding of bt_ctrl for make test-bt-host: the bt_ctrl task (OPENFW_PRIO_BT_CTRL) and the bt_dlv task
 * (OPENFW_PRIO_BT_DLV) on the FreeRTOS POSIX port, tests/fake_rom.c as the ROM, and the
 * esp_bluedroid_hci_driver_operations_t that Bluedroid's hciT calls. Same shape as bt_port/bt_ctrl_rtos.c (which
 * needs SL6801 registers), minus the probe guards.
 *
 * fake_rom is single-threaded: the bt_ctrl task holds s_rom_mutex around bt_ctrl_run() (its only ROM calls) and the
 * test task takes it around any fake_rom_* call (bth_rom_lock), so the model sees one caller at a time.
 * SPDX-License-Identifier: MIT */
#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "openfw_rtos.h"
#include "esp_bluedroid_hci.h"
#include "bt_ctrl.h"
#include "fake_rom.h"
#include "host_env.h"
#include "bta/bta_api.h"
#include "bta_dm_int.h"

static TaskHandle_t s_ctrl_task, s_dlv_task;
static SemaphoreHandle_t s_rom_mutex;
static volatile uint32_t s_faults, s_first_fault, s_dlv_retries;
static bool s_stop;
static const uint8_t k_bd_addr[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0xC6 };

/* esp_bluedroid_hci_driver_callbacks_t and bt_ctrl_host_cb_t are the same table (bt_ctrl.h). */
_Static_assert(sizeof(esp_bluedroid_hci_driver_callbacks_t) == sizeof(bt_ctrl_host_cb_t), "callback table shape");
_Static_assert(offsetof(esp_bluedroid_hci_driver_callbacks_t, notify_host_send_available) ==
               offsetof(bt_ctrl_host_cb_t, notify_host_send_available), "callback table shape");
_Static_assert(offsetof(esp_bluedroid_hci_driver_callbacks_t, notify_host_recv) ==
               offsetof(bt_ctrl_host_cb_t, notify_host_recv), "callback table shape");

bool bth_forget_dm_peer(void)
{
    taskENTER_CRITICAL();
    bool found = BTM_GetNumAclLinks() == 1u && bta_dm_cb.device_list.count == 1u;
    if (found) {
        bta_dm_cb.device_list.count = 0u;
    }
    taskEXIT_CRITICAL();
    return found;
}

static void plat_enter(void) { taskENTER_CRITICAL(); }
static void plat_exit(void) { taskEXIT_CRITICAL(); }
static void plat_wake_ctrl(void)
{
    if (s_ctrl_task != NULL) {
        xTaskNotifyGive(s_ctrl_task);
    }
}
/* fake_rom raises "IRQ 26" from the test task (task context), so the task-level notify is the right call here. */
static void plat_wake_ctrl_from_isr(void) { plat_wake_ctrl(); }
static void plat_wake_dlv(void)
{
    if (s_dlv_task != NULL) {
        xTaskNotifyGive(s_dlv_task);
    }
}
static bool plat_stop_requested(void) { return s_stop; }
static void plat_fault(uint32_t code)
{
    taskENTER_CRITICAL();
    s_faults++;
    if (s_first_fault == 0u) {
        s_first_fault = code;
    }
    taskEXIT_CRITICAL();
}
static const bt_ctrl_platform_t k_platform = {
    plat_enter, plat_exit, plat_wake_ctrl, plat_wake_ctrl_from_isr, plat_wake_dlv, plat_stop_requested, plat_fault,
};

static void bt_ctrl_task(void *arg)
{
    (void)arg;
    uint32_t more = 0u;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, more ? 0u : portMAX_DELAY);
        (void)xSemaphoreTake(s_rom_mutex, portMAX_DELAY);
        more = bt_ctrl_run();
        (void)xSemaphoreGive(s_rom_mutex);
        if (more) {
            taskYIELD();
        }
    }
}

static void bt_dlv_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (bt_ctrl_dlv_run() == BT_CTRL_DLV_RETRY) {
            s_dlv_retries++;
            vTaskDelay(1);                       /* plan M1: retry with a 1-tick backoff */
        }
    }
}

/* ---- HCI driver ops (hciT / BTC) ---- */
static void ops_send(uint8_t *data, uint16_t len) { bt_ctrl_send(data, len); }
static bool ops_check(void) { return bt_ctrl_check_send_available(); }
static esp_err_t ops_register(const esp_bluedroid_hci_driver_callbacks_t *cb)
{
    return bt_ctrl_register_host_callback((const bt_ctrl_host_cb_t *)cb) == 0 ? ESP_OK : ESP_FAIL;
}
static const esp_bluedroid_hci_driver_operations_t k_ops = { ops_send, ops_check, ops_register };

int bth_ctrl_init(void)
{
    if (s_ctrl_task != NULL) {
        return 0;
    }
    s_rom_mutex = xSemaphoreCreateMutex();
    if (s_rom_mutex == NULL) {
        return -1;
    }
    fake_rom_reset();
    fake_rom.stop_flag = &s_stop;
    fake_rom.heap_budget = 1u << 20;     /* g_bt_heap budget is a bt_ctrl unit-test subject, not this harness's */
    int err = bt_ctrl_init(&fake_rom_if, &k_platform, k_bd_addr);
    if (xTaskCreate(bt_ctrl_task, "bt_ctrl", 8192u, NULL, OPENFW_PRIO_BT_CTRL, &s_ctrl_task) != pdPASS ||
        xTaskCreate(bt_dlv_task, "bt_dlv", 8192u, NULL, OPENFW_PRIO_BT_DLV, &s_dlv_task) != pdPASS) {
        return -1;
    }
    return err;
}

int bth_ctrl_open(void)
{
    if (bt_ctrl_open() != 0) {
        return -1;
    }
    return esp_bluedroid_attach_hci_driver(&k_ops) == ESP_OK ? 0 : -2;
}

int bth_ctrl_close(uint32_t timeout_ms)
{
    bt_ctrl_shutdown_begin();
    plat_wake_ctrl();
    for (uint32_t t = 0u; t < timeout_ms; t++) {
        if (bt_ctrl_shutdown_status() != BT_CTRL_STOP_PENDING && !bt_ctrl_dlv_busy()) {
            return bt_ctrl_shutdown_status();
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return BT_CTRL_STOP_PENDING;
}

void bth_ctrl_kick(void) { plat_wake_ctrl(); }
void bth_rom_lock(void) { (void)xSemaphoreTake(s_rom_mutex, portMAX_DELAY); }
void bth_rom_unlock(void) { (void)xSemaphoreGive(s_rom_mutex); }
uint32_t bth_ctrl_faults(void) { return s_faults; }
uint32_t bth_ctrl_first_fault(void) { return s_first_fault; }
uint32_t bth_dlv_retries(void) { return s_dlv_retries; }
