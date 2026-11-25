/* Targeted out-of-memory levers for the phone scenario (review STEP10 A9 / C7 / C11, patch 0044).
 *
 * The count-based injection of host_heap.c cannot single out one allocation on a busy connection, so these wrap the
 * three Bluedroid entry points whose allocation failures 0044 handles (linked with --wrap, see Makefile WRAPS):
 *   btc_transfer_context()          a chosen BTA -> BTC hand-off or SPP API post fails as if its message could not be
 *                                   allocated (BT_STATUS_NOMEM, nothing allocated or freed);
 *   fixed_queue_enqueue()           on BTU_TASK only: the list-node allocation of the next enqueue fails (false, the
 *                                   item untouched: patch 0009 semantics). On an idle connection the first BTU enqueue
 *                                   after the laptop sends is btc_spp's RX queue (bta_co_rfc_data_incoming);
 *   btc_storage_add_bonded_device() the link key cannot be stored (BT_STATUS_FAIL before any config write);
 *   (host_heap.c)                   on BTC_TASK, the next allocation of sizeof(tBTA_JV_FREE_SCN_USER_DATA): during
 *                                   esp_spp_deinit() that is btc_spp_uninit()'s FreeChannel user data.
 * Each lever fails the next n matching calls and counts them. TEST ONLY.
 * SPDX-License-Identifier: MIT */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "btc/btc_task.h"
#include "btc/btc_storage.h"
#include "bta/bta_jv_api.h"
#include "btc_spp.h"
#include "osi/fixed_queue.h"
#include "host_env.h"

static int s_left[BTH_OOM_N];
static uint32_t s_hits[BTH_OOM_N];

static bool take(int what)
{
    bool hit = false;
    taskENTER_CRITICAL();
    if (s_left[what] > 0) {
        s_left[what]--;
        s_hits[what]++;
        hit = true;
    }
    taskEXIT_CRITICAL();
    return hit;
}

void bth_oom_arm(int what, int n)
{
    if (what < 0 || what >= BTH_OOM_N) {
        return;
    }
    taskENTER_CRITICAL();
    s_left[what] = n;
    taskEXIT_CRITICAL();
}

uint32_t bth_oom_hits(int what)
{
    if (what < 0 || what >= BTH_OOM_N) {
        return 0u;
    }
    taskENTER_CRITICAL();
    uint32_t h = s_hits[what];
    taskEXIT_CRITICAL();
    return h;
}

bool bth_oom_take_alloc(size_t n)
{
    return n == sizeof(tBTA_JV_FREE_SCN_USER_DATA) && s_left[BTH_OOM_BTC_FREE_SCN_UD] > 0 &&
           xTaskGetSchedulerState() == taskSCHEDULER_RUNNING && strcmp(pcTaskGetName(NULL), "BTC_TASK") == 0 &&
           take(BTH_OOM_BTC_FREE_SCN_UD);
}

bt_status_t __real_btc_transfer_context(btc_msg_t *msg, void *arg, int arg_len, btc_arg_deep_copy_t copy_func,
                                        btc_arg_deep_free_t free_func);
bt_status_t __wrap_btc_transfer_context(btc_msg_t *msg, void *arg, int arg_len, btc_arg_deep_copy_t copy_func,
                                        btc_arg_deep_free_t free_func);
bt_status_t __wrap_btc_transfer_context(btc_msg_t *msg, void *arg, int arg_len, btc_arg_deep_copy_t copy_func,
                                        btc_arg_deep_free_t free_func)
{
    if (msg != NULL && msg->pid == BTC_PID_SPP) {
        int what = -1;
        if (msg->sig == BTC_SIG_API_CB && msg->act == BTA_JV_RFCOMM_WRITE_EVT) {
            what = BTH_OOM_SPP_WRITE_CMPL;
        } else if (msg->sig == BTC_SIG_API_CB && msg->act == BTA_JV_RFCOMM_DATA_IND_EVT) {
            what = BTH_OOM_SPP_DATA_IND;
        } else if (msg->sig == BTC_SIG_API_CALL && msg->act == BTC_SPP_ACT_DISCONNECT) {
            what = BTH_OOM_SPP_DISCONNECT;
        }
        if (what >= 0 && take(what)) {
            return BT_STATUS_NOMEM;
        }
    }
    return __real_btc_transfer_context(msg, arg, arg_len, copy_func, free_func);
}

bool __real_fixed_queue_enqueue(fixed_queue_t *queue, void *data, uint32_t timeout);
bool __wrap_fixed_queue_enqueue(fixed_queue_t *queue, void *data, uint32_t timeout);
bool __wrap_fixed_queue_enqueue(fixed_queue_t *queue, void *data, uint32_t timeout)
{
    if (s_left[BTH_OOM_BTU_ENQUEUE] > 0 && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING &&
            strcmp(pcTaskGetName(NULL), "BTU_TASK") == 0 && take(BTH_OOM_BTU_ENQUEUE)) {
        return false;
    }
    return __real_fixed_queue_enqueue(queue, data, timeout);
}

bt_status_t __real_btc_storage_add_bonded_device(bt_bdaddr_t *remote_bd_addr, LINK_KEY link_key, uint8_t key_type,
                                                 uint8_t pin_length, BOOLEAN sc_support);
bt_status_t __wrap_btc_storage_add_bonded_device(bt_bdaddr_t *remote_bd_addr, LINK_KEY link_key, uint8_t key_type,
                                                 uint8_t pin_length, BOOLEAN sc_support);
bt_status_t __wrap_btc_storage_add_bonded_device(bt_bdaddr_t *remote_bd_addr, LINK_KEY link_key, uint8_t key_type,
                                                 uint8_t pin_length, BOOLEAN sc_support)
{
    if (take(BTH_OOM_BOND_STORE)) {
        return BT_STATUS_FAIL;
    }
    return __real_btc_storage_add_bonded_device(remote_bd_addr, link_key, key_type, pin_length, sc_support);
}
