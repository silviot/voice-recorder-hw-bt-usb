/* Firmware platform for bt_xrec_bluedroid (BT=1 phone mode, plan M4): device callbacks for xrec_proto, the bond store
 * on SD, stack bring-up / teardown jobs in the bt_recover worker, the hard-stop deadline, the hard path.
 *
 * F: = src/bt_hci.c, M: = src/main.c (line numbers of this tree). The device callbacks are the legacy ones
 * (F:1733-1966) with the session's volume; the legacy file keeps its own static copies (renamed and unreferenced in
 * BT=1 images, open_fw/Makefile), which is why they are repeated here rather than shared.
 *
 * Contexts: everything runs in the app task except
 *   - the bt_recover worker (priority 2, static TCB/stack in SRAM_HI, created once): up job (bt_ctrl open + HCI
 *     driver attach + esp_bluedroid_init + enable) and down job (esp_spp_deinit + disable + bt_ctrl stop + deinit).
 *     It may block forever inside Bluedroid; the app never waits for it without the absolute deadline (plan M1).
 *   - two FreeRTOS timer-daemon callbacks (priority 9, preempt the app even inside a long SD write): the one-shot
 *     teardown deadline (latches bt_stop and masks IRQ 26 at 2 s) and the 100 ms bt_ctrl hang guard.
 *   - store_flush_request(), called by bt_store from the committing BT task: sets a flag and wakes the app.
 * SPDX-License-Identifier: MIT */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "sl6801_regs.h"
#include "sl6801_rom.h"
#include "openfw.h"
#include "openfw_exp.h"
#include "openfw_rtos.h"
#include "esp_err.h"
#include "esp_bt_main.h"
#include "esp_bluedroid_hci.h"
#include "esp_spp_api.h"
#include "bt_port.h"
#include "bt_store.h"
#include "bt_entropy.h"
#include "bt_ctrl.h"
#include "bt_ctrl_rtos.h"
#include "bt_rom_real.h"
#include "bt_xrec_bluedroid.h"

/* Static, heap-free objects of the recovery ladder (plan M1: bt_recover exists without heap). They live in SRAM_HI
 * (.bss_hi, zeroed at boot, like bt_port/hci_probe.c's buffers) rather than .rtos_static: with bt_ctrl's rings and
 * task stacks linked, the RTOS_HEAP region would drop below the 32 KiB heap_5 floor asserted by sl6801_firm.ld. */
#define BT_HI_STATIC __attribute__((section(".bss_hi"), aligned(8)))
#define BT_RECOVER_PRIO         2u        /* plan §3 */
#define BT_RECOVER_STACK_WORDS  768u      /* runs esp_bluedroid_init/enable/disable/deinit; watermark logged at stop */
#define BT_GUARD_PERIOD_MS      100u
#define BT_HANG_LIMIT_US        3000000u  /* one bt_ctrl_run() longer than this = stuck in the ROM (probe value) */
#define BT_FAULT_CHECK_MS       200u
#define BT_FAULT_RECORD_CODE    0xB1u     /* plan M1: AON fault record, BT stays off on the next boot */
#define BOND_FILE               "bt_bonds.bin"
#define BOND_TMP                ".bt_bonds.tmp"
#define BOND_RETRY_MS           1000u     /* a failed bond-file write is retried this often ... */
#define BOND_RETRY_MAX          8u        /* ... this many times per session (then at the next change / session end) */

/* ---- session ---- */
static exfat_vol_t *s_vol;
static int s_sd;
static uint8_t s_bd[6];
static TaskHandle_t s_app;

/* ---- worker ---- */
enum { JOB_NONE = 0, JOB_UP, JOB_DOWN };
static StaticTask_t s_rec_tcb BT_HI_STATIC;
static StackType_t s_rec_stack[BT_RECOVER_STACK_WORDS] BT_HI_STATIC;
static TaskHandle_t s_rec_task;
static volatile uint32_t s_req_seq, s_done_seq;
static volatile uint8_t s_req_job;
static volatile int s_done_res;

/* ---- timers ---- */
static StaticTimer_t s_dl_buf BT_HI_STATIC;
static StaticTimer_t s_guard_buf BT_HI_STATIC;
static TimerHandle_t s_dl_timer, s_guard_timer;
static volatile uint8_t s_dl_armed, s_dl_fired, s_ctrl_hung;

/* ---- faults / telemetry ---- */
/* RAM budget telemetry (plan §4: free heap under traffic >= 8 KiB, largest free block >= the largest allocation;
 * review finding: nothing measured it on silicon). Stack high-water marks are sampled each second while BT runs (the
 * Bluedroid tasks exist only then) and logged with the heap figures every 30 s and at "BT stopped". */
#define BT_MEM_SAMPLE_MS        1000u
#define BT_MEM_LOG_MS           30000u
#define BT_MEM_NTASKS           7u
static const char *const k_mem_tasks[BT_MEM_NTASKS] = {
    "BTU_TASK", "BTC_TASK", "hciT", "bt_ctrl", "bt_dlv", "bt_recover", "app",
};
static uint32_t s_mem_wm[BT_MEM_NTASKS];   /* lowest stack high-water mark this session, words; UINT32_MAX = unseen */
static uint32_t s_mem_sample_at, s_mem_log_at;
extern volatile uint32_t g_rtos_malloc_failures;    /* src/rtos/rtos_port.c (BT=1: failures are counted, not fatal) */
static volatile uint8_t s_store_flush_req;
static uint8_t s_bond_fails;             /* consecutive failed bond-file writes (store kept dirty) */
static uint32_t s_bond_retry_at;
static uint32_t s_last_check, s_faults_base, s_hci_to_base, s_irq_cache;

/* ============================================ small helpers ===================================================== */
uint32_t bt_xrec_plat_now_ms(void) { return millis(); }
void bt_xrec_plat_lock(void) { taskENTER_CRITICAL(); }
void bt_xrec_plat_unlock(void) { taskEXIT_CRITICAL(); }

void bt_xrec_plat_notify_app(void)
{
    if (s_app != NULL) {
        xTaskNotifyGive(s_app);      /* wakes OPENFW_TASK_WAIT() in the BT-mode loop (M:1393) */
    }
}

void bt_xrec_plat_log(const char *msg)
{
    openfw_log_puts(msg);
    openfw_log_putc('\n');
}

void bt_xrec_plat_log_u32(const char *msg, uint32_t v)
{
    openfw_log_puts(msg);
    openfw_log_putc(' ');
    openfw_log_u32(v);
    openfw_log_putc('\n');
}

void *bt_xrec_plat_alloc(size_t n) { return pvPortMalloc(n); }
void bt_xrec_plat_free(void *p) { vPortFree(p); }
void bt_xrec_plat_service(void) { recorder_poll(); }
void bt_xrec_plat_wait(void) { vTaskDelay(1); }
uint32_t bt_xrec_plat_irq_count(void) { return s_irq_cache; }

int bt_xrec_plat_start_allowed(void)
{
    return g_prev_fault_code != (OPENFW_FAULT_MAGIC_BASE | BT_FAULT_RECORD_CODE);
}

/* Stable per-device identity (plan M1): eFuse BD_ADDR (02:CA:55:00:01:1D on the bench unit, BT_HW_RESULTS session 1)
 * with the legacy layout F:1990-2003; without eFuse a fixed address (bt_port/hci_probe.c probe_bd_addr) instead of
 * the legacy timer-derived one, so bonds survive a reboot. NOR-UID derivation is a follow-up. */
void bt_xrec_plat_identity(uint8_t bd[6])
{
    rom_rst_release(0x58u);          /* eFuse block reset/clock, not RW-BT's (BT_HW_RESULTS lessons) */
    rom_clk_enable(0x1Bu);
    bool efuse = (EFUSE_WORD(3) & (1u << 31)) != 0u;
    uint32_t ef0 = efuse ? EFUSE_WORD(0) : 0x0001u;
    uint32_t ef1 = efuse ? EFUSE_WORD(1) : 0x00u;
    bd[0] = (uint8_t)(ef0 & 0xFFu);
    bd[1] = (uint8_t)((ef0 >> 8) & 0xFFu);
    bd[2] = (uint8_t)(ef1 & 0xFFu);
    bd[3] = 0x55u;
    bd[4] = 0xCAu;
    bd[5] = 0x02u;                   /* locally administered, unicast */
}

int bt_xrec_plat_entropy(void *out, size_t n)
{
    return bt_entropy_get(out, n) == BT_ENT_OK ? 0 : -1;
}

/* ============================================ files (app task) ================================================== */
/* Staged replace, as legacy save_root_file_atomic (F:1441-1462). Returns 0 on success. */
static int save_root_file_atomic(const char *tmp_name, const char *final_name, const void *data, uint32_t len)
{
    if (!s_sd || s_vol == NULL) {
        return -1;
    }
    rtc_datetime_t dt;
    rtc_get(&dt);
    exfat_set_timestamp(s_vol, rtc_to_dos_timestamp(&dt));
    (void)exfat_delete_file(s_vol, "/", tmp_name);
    exfat_writer_t w;
    if (exfat_create_file(s_vol, "/", tmp_name, &w) == 0) {
        int wrc = exfat_write(&w, data, len);
        int crc = exfat_close_file(&w);
        if (wrc == 0 && crc == 0 && exfat_replace_file(s_vol, "/", tmp_name, final_name) == 0) {
            return 0;
        }
        (void)exfat_delete_file(s_vol, "/", tmp_name);
    }
    return -1;
}

/* Called by bt_xrec_start() BEFORE bt_xrec_plat_stack_up_begin() has set s_vol / s_sd, so it reads the session's
 * volume from the context (xrec_init() stored it, NULL without SD). Using s_vol here never loaded the key on the first
 * start of a boot (found by the test-bt-host phone scenario's restart step). */
void bt_xrec_plat_load_bound_key(xrec_ctx_t *ctx)
{
    exfat_vol_t *vol = ctx->vol;
    if (vol == NULL) {
        return;
    }
    exfat_entry_t ent;
    if (exfat_find_path(vol, "/cassini.key", &ent) != 0 || ent.is_dir || ent.size != 36u) {
        return;
    }
    uint8_t blob[36];
    uint32_t nread = 0u;
    if (exfat_read_file(vol, &ent, 0u, blob, (uint32_t)sizeof(blob), &nread) == 0 && nread == 36u &&
        blob[0] == 'C' && blob[1] == 'S' && blob[2] == 'K' && blob[3] == '5') {
        xrec_set_bound_key(ctx, &blob[4]);
    }
    memset(blob, 0, sizeof(blob));
}

void bt_xrec_plat_save_bound_key(const uint8_t key[32])
{
    uint8_t blob[36];
    blob[0] = 'C';
    blob[1] = 'S';
    blob[2] = 'K';
    blob[3] = '5';
    memcpy(&blob[4], key, 32u);
    if (save_root_file_atomic(".cassini.tmp", "cassini.key", blob, (uint32_t)sizeof(blob)) != 0 && s_sd) {
        bt_xrec_plat_log("BT XREC key not saved (SD write failed)");
    }
    memset(blob, 0, sizeof(blob));
}

/* Bond store (plan M2 "Store"): loaded before esp_bluedroid_init(); written by the app when bt_store asks. */
static void bonds_load(void)
{
    if (!s_sd || s_vol == NULL) {
        return;                      /* no SD: RAM-only bonds for the session (whatever this boot already holds) */
    }
    exfat_entry_t ent;
    if (exfat_find_path(s_vol, "/" BOND_FILE, &ent) != 0 || ent.is_dir || ent.size == 0u ||
        ent.size > BT_STORE_IMAGE_MAX) {
        return;
    }
    uint8_t *img = (uint8_t *)pvPortMalloc((size_t)ent.size);
    if (img == NULL) {
        bt_xrec_plat_log("BT bonds not loaded: no memory");
        return;
    }
    uint32_t nread = 0u;
    if (exfat_read_file(s_vol, &ent, 0u, img, (uint32_t)ent.size, &nread) == 0 && nread == ent.size) {
        esp_err_t r = bt_store_load(img, nread);
        if (r != ESP_OK) {
            bt_xrec_plat_log_u32("BT bond file rejected (store starts empty), err", (uint32_t)r);
        }
    }
    memset(img, 0, (size_t)ent.size);
    vPortFree(img);
}

static void bonds_write(void)
{
    if (!bt_store_dirty() || !s_sd || s_vol == NULL) {
        return;
    }
    uint8_t *img = (uint8_t *)pvPortMalloc(BT_STORE_IMAGE_MAX);
    if (img == NULL) {
        s_store_flush_req = 1u;      /* retry on a later poll */
        return;
    }
    size_t n = bt_store_serialize(img, BT_STORE_IMAGE_MAX);
    if (n != 0u && save_root_file_atomic(BOND_TMP, BOND_FILE, img, (uint32_t)n) == 0) {
        bt_store_mark_saved();       /* only now: a failed write keeps the store dirty (review finding) */
        s_bond_fails = 0u;
    } else {
        bt_xrec_plat_log("BT bonds not saved (SD write failed, retrying)");
        if (s_bond_fails < BOND_RETRY_MAX) {
            s_bond_fails++;
        }
        s_bond_retry_at = millis() + BOND_RETRY_MS;
    }
    memset(img, 0, BT_STORE_IMAGE_MAX);
    vPortFree(img);
}

static void store_flush_request(void)
{
    s_store_flush_req = 1u;
    bt_xrec_plat_notify_app();
}

/* ============================================ xrec_proto device callbacks ======================================= */
static void dev_get_stat(void *user, xrec_stat_t *out)       /* F:1733-1752 */
{
    (void)user;
    uint32_t mv = pmu_read_vbat_mv();
    uint32_t pct = 0u;
    if (mv > 3350u) {
        pct = (mv >= 4150u) ? 100u : ((mv - 3350u) * 100u) / 800u;
    }
    out->voltage_mv = (uint16_t)mv;
    out->bat_pct = (uint8_t)pct;
    out->charge_status = (uint8_t)pmu_get_charge_state();
    out->vbus = (uint8_t)(pmu_is_vbus_present() ? 1u : 0u);
    out->media_status = recorder_is_saving() ? 0x21u : 0x10u;
    out->rec_sec = recorder_is_saving() ? recorder_get_seconds() : 0u;
    out->free_mb = 0u;
    out->total_mb = 0u;
    if (s_sd && s_vol != NULL) {
        (void)exfat_stat_fs(s_vol, &out->free_mb, &out->total_mb);
    }
}

static void dev_get_rtc(void *user, xrec_rtc_t *out)         /* F:1754-1766 */
{
    (void)user;
    rtc_datetime_t dt;
    rtc_get(&dt);
    out->year = dt.year;
    out->month = dt.month;
    out->day = dt.day;
    out->hour = dt.hour;
    out->minute = dt.minute;
    out->second = dt.second;
    out->weekday = dt.weekday;
}

static int dev_set_rtc(void *user, const xrec_rtc_t *rt)     /* F:1768-1786 */
{
    (void)user;
    rtc_datetime_t dt;
    dt.year = rt->year;
    dt.month = rt->month;
    dt.day = rt->day;
    dt.hour = rt->hour;
    dt.minute = rt->minute;
    dt.second = rt->second;
    dt.weekday = rt->weekday;
    rtc_set(&dt);
    if (s_sd && s_vol != NULL) {
        rtc_datetime_t now_dt;
        rtc_get(&now_dt);
        exfat_set_timestamp(s_vol, rtc_to_dos_timestamp(&now_dt));
    }
    return 0;
}

static int dev_rec_start(void *user, const char *opt_name, char *out_name, uint32_t out_max)   /* F:1934-1960 */
{
    (void)user;
    (void)opt_name;
    if (out_name == NULL || out_max == 0u) {
        return -1;
    }
    out_name[0] = '\0';
    int was_saving = recorder_is_saving();
    if (recorder_start(s_vol, s_sd) != 0) {
        return -1;
    }
    if (recorder_is_saving()) {
        const char *fname = recorder_get_filename();
        uint32_t i = 0u;
        while (fname[i] != '\0' && (i + 1u) < out_max) {
            out_name[i] = fname[i];
            i++;
        }
        out_name[i] = '\0';
        return (i > 0u) ? 0 : -1;
    }
    if (!was_saving && !keys_read_rec_switch()) {
        (void)recorder_stop();
    }
    return -1;
}

static int dev_rec_stop(void *user)
{
    (void)user;
    return recorder_stop();
}

#if OPENFW_ENABLE_USB_EXP
static int dev_exp_write(void *user, uint32_t off, const uint8_t *data, uint16_t len)   /* F:1838-1858 */
{
    (void)user;
    return openfw_exp_write(off, data, len);
}

static int dev_exp_run(void *user, uint32_t entry_off, uint32_t arg, uint32_t *rc, uint32_t *elapsed_ms,
                       uint32_t *out_len, uint32_t *log_widx)
{
    (void)user;
    openfw_exp_result_t res;
    if (openfw_exp_run(entry_off, arg, &res) != 0) {
        return -1;
    }
    if (rc) *rc = res.rc;
    if (elapsed_ms) *elapsed_ms = res.elapsed_ms;
    if (out_len) *out_len = res.out_len;
    if (log_widx) *log_widx = res.log_widx;
    return 0;
}
#endif

static uint32_t dev_read_log(void *user, uint32_t off, uint8_t *dst, uint32_t max_len, uint32_t *out_widx)  /* F:1861 */
{
    (void)user;
    if (g_openfw_log.magic != OPENFW_LOG_MAGIC || g_openfw_log.size != OPENFW_LOG_SIZE) {
        if (out_widx) {
            *out_widx = 0u;
        }
        return 0u;
    }
    uint32_t widx = g_openfw_log.write_idx;
    uint32_t avail = (widx < OPENFW_LOG_SIZE) ? widx : OPENFW_LOG_SIZE;
    if (out_widx) {
        *out_widx = widx;
    }
    if (dst == NULL || max_len == 0u || off >= avail) {
        return 0u;
    }
    uint32_t start = (widx <= OPENFW_LOG_SIZE) ? 0u : (widx & (OPENFW_LOG_SIZE - 1u));
    uint32_t n = avail - off;
    if (n > max_len) {
        n = max_len;
    }
    for (uint32_t i = 0u; i < n; i++) {
        dst[i] = (uint8_t)g_openfw_log.buf[(start + off + i) & (OPENFW_LOG_SIZE - 1u)];
    }
    return n;
}

static void dev_reset_log(void *user)
{
    (void)user;
    openfw_log_reset();
}

static int dev_audio_cfg(void *user, int set, uint8_t *dsp_profile, uint8_t *gain, uint8_t *channels, uint8_t *vor)
{                                                              /* F:1903-1926 */
    (void)user;
    if (set) {
        if (dsp_profile && *dsp_profile <= 2u) {
            recorder_set_dsp_profile((audio_dsp_profile_t)*dsp_profile);
        }
        if (gain && *gain <= 31u) {
            recorder_set_gain(*gain);
        }
        if (channels && (*channels == 1u || *channels == 2u)) {
            recorder_set_channels(*channels);
        }
        if (vor && *vor <= 1u) {
            recorder_set_vor((int)*vor);
        }
    }
    if (dsp_profile) *dsp_profile = (uint8_t)recorder_get_dsp_profile();
    if (gain) *gain = recorder_get_gain();
    if (channels) *channels = recorder_get_channels();
    if (vor) *vor = (uint8_t)(recorder_get_vor() ? 1u : 0u);
    return 0;
}

/* send / get_entropy / on_bound_key / on_reboot / on_ota / on_dfu / get_mtu are filled in by the session. */
static const xrec_ops_t k_dev_ops = {
    .get_stat = dev_get_stat,
    .get_rtc = dev_get_rtc,
    .set_rtc = dev_set_rtc,
    .rec_start = dev_rec_start,
    .rec_stop = dev_rec_stop,
#if OPENFW_ENABLE_USB_EXP
    .exp_write = dev_exp_write,
    .exp_run = dev_exp_run,
#endif
    .read_log = dev_read_log,
    .reset_log = dev_reset_log,
    .audio_cfg = dev_audio_cfg,
};

const xrec_ops_t *bt_xrec_plat_device_ops(void) { return &k_dev_ops; }

/* ============================================ RAM telemetry (app task) ========================================== */
static void mem_reset(void)
{
    for (uint32_t i = 0u; i < BT_MEM_NTASKS; i++) {
        s_mem_wm[i] = UINT32_MAX;
    }
    s_mem_sample_at = s_mem_log_at = millis();
}

static void mem_sample(void)
{
    for (uint32_t i = 0u; i < BT_MEM_NTASKS; i++) {
        TaskHandle_t h = xTaskGetHandle(k_mem_tasks[i]);
        if (h != NULL) {
            uint32_t w = (uint32_t)uxTaskGetStackHighWaterMark(h);
            if (w < s_mem_wm[i]) {
                s_mem_wm[i] = w;
            }
        }
    }
}

/* "BT mem <when> heap free F min M largest L mallocfail N" and "BT stack free words BTU_TASK=w ... app=w" (- = the
 * task did not run this session). */
static void mem_log(const char *when)
{
    HeapStats_t hs;
    memset(&hs, 0, sizeof(hs));
    vPortGetHeapStats(&hs);
    openfw_log_puts("BT mem ");
    openfw_log_puts(when);
    openfw_log_puts(" heap free ");
    openfw_log_u32((uint32_t)hs.xAvailableHeapSpaceInBytes);
    openfw_log_puts(" min ");
    openfw_log_u32((uint32_t)xPortGetMinimumEverFreeHeapSize());
    openfw_log_puts(" largest ");
    openfw_log_u32((uint32_t)hs.xSizeOfLargestFreeBlockInBytes);
    openfw_log_puts(" mallocfail ");
    openfw_log_u32(g_rtos_malloc_failures);
    openfw_log_putc('\n');
    openfw_log_puts("BT stack free words");
    for (uint32_t i = 0u; i < BT_MEM_NTASKS; i++) {
        openfw_log_putc(' ');
        openfw_log_puts(k_mem_tasks[i]);
        openfw_log_putc('=');
        if (s_mem_wm[i] == UINT32_MAX) {
            openfw_log_putc('-');
        } else {
            openfw_log_u32(s_mem_wm[i]);
        }
    }
    openfw_log_putc('\n');
}

/* ============================================ HCI driver (hciT -> bt_ctrl) ====================================== */
/* esp_bluedroid_hci_driver_callbacks_t and bt_ctrl_host_cb_t are the same table (bt_ctrl.h; tests/bt_host/host_ctrl.c). */
_Static_assert(sizeof(esp_bluedroid_hci_driver_callbacks_t) == sizeof(bt_ctrl_host_cb_t), "callback table shape");
_Static_assert(offsetof(esp_bluedroid_hci_driver_callbacks_t, notify_host_send_available) ==
               offsetof(bt_ctrl_host_cb_t, notify_host_send_available), "callback table shape");
_Static_assert(offsetof(esp_bluedroid_hci_driver_callbacks_t, notify_host_recv) ==
               offsetof(bt_ctrl_host_cb_t, notify_host_recv), "callback table shape");

static void hci_send(uint8_t *data, uint16_t len) { bt_ctrl_send(data, len); }
static bool hci_check(void) { return bt_ctrl_check_send_available(); }
static esp_err_t hci_register(const esp_bluedroid_hci_driver_callbacks_t *cb)
{
    return bt_ctrl_register_host_callback((const bt_ctrl_host_cb_t *)cb) == 0 ? ESP_OK : ESP_FAIL;
}
static const esp_bluedroid_hci_driver_operations_t k_hci_ops = { hci_send, hci_check, hci_register };

/* ============================================ worker jobs ======================================================= */
static int job_up(void)
{
    if (bt_ctrl_open() != 0) {
        return 1;
    }
    if (esp_bluedroid_attach_hci_driver(&k_hci_ops) != ESP_OK) {
        return 2;
    }
    if (esp_bluedroid_init() != ESP_OK) {
        return 3;
    }
    if (esp_bluedroid_enable() != ESP_OK) {      /* HCI Reset (RF calibration + ROM start) .. init chain */
        return 4;
    }
    return 0;
}

/* Plan M1 teardown order: profile close + Bluedroid disable with the transport alive -> bt_ctrl stop (callbacks
 * quiesced, admission closed, controller stopped, IRQ 26 masked) once bt_dlv left the host -> deinit. Any wait may
 * block forever; the app's deadline covers it. */
static int job_down(void)
{
    int rc = 0;
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) {
        esp_spp_profile_status_t ps;
        memset(&ps, 0, sizeof(ps));
        if (esp_spp_get_profile_status(&ps) == ESP_OK && ps.spp_inited) {
            if (esp_spp_deinit() == ESP_OK) {
                while (!bt_xrec_spp_uninit_seen()) {
                    vTaskDelay(1);
                }
            } else {
                rc = 1;
            }
        }
        if (esp_bluedroid_disable() != ESP_OK) {
            rc = 2;
        }
    }
    bt_ctrl_shutdown_begin();
    while (bt_ctrl_shutdown_status() == BT_CTRL_STOP_PENDING || bt_ctrl_dlv_busy()) {
        vTaskDelay(1);
    }
    if (bt_ctrl_shutdown_status() != BT_CTRL_STOP_OK) {
        rc = 3;
    }
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_UNINITIALIZED && esp_bluedroid_deinit() != ESP_OK) {
        rc = 4;
    }
    return rc;
}

static void bt_recover_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
        taskENTER_CRITICAL();
        uint32_t seq = s_req_seq;
        uint8_t job = s_req_job;
        taskEXIT_CRITICAL();
        if (seq == s_done_seq) {
            continue;                /* a later request already covered this notification */
        }
        int r = job == JOB_UP ? job_up() : job_down();
        taskENTER_CRITICAL();
        s_done_res = r;
        s_done_seq = seq;
        taskEXIT_CRITICAL();
        bt_xrec_plat_notify_app();
    }
}

static void post_job(uint8_t job)
{
    taskENTER_CRITICAL();
    s_req_job = job;
    s_req_seq++;
    taskEXIT_CRITICAL();
    xTaskNotifyGive(s_rec_task);
}

int bt_xrec_plat_job_done(int *result)
{
    taskENTER_CRITICAL();
    int done = s_done_seq == s_req_seq;
    int r = s_done_res;
    taskEXIT_CRITICAL();
    if (done && result != NULL) {
        *result = r;
    }
    return done;
}

/* ============================================ timers (daemon, priority 9) ======================================= */
static void deadline_cb(TimerHandle_t t)
{
    (void)t;
    bool fire = false;
    taskENTER_CRITICAL();
    if (s_dl_armed) {
        s_dl_armed = 0u;
        s_dl_fired = 1u;
        fire = true;
    }
    taskEXIT_CRITICAL();
    if (fire) {
        bt_ctrl_rtos_hard_stop();    /* plan M1: IRQ 26 masked + bt_stop, independent of the app */
    }
}

static void guard_cb(TimerHandle_t t)
{
    (void)t;
    if (bt_ctrl_rtos_guard(BT_HANG_LIMIT_US)) {
        s_ctrl_hung = 1u;
    }
}

int bt_xrec_plat_deadline_disarm(void)
{
    taskENTER_CRITICAL();
    s_dl_armed = 0u;
    int fired = s_dl_fired;
    taskEXIT_CRITICAL();
    (void)xTimerStop(s_dl_timer, 0);
    return fired;
}

/* ============================================ lifecycle (app task) ============================================== */
static int once_init(void)
{
    if (s_rec_task != NULL) {
        return 0;
    }
    s_dl_timer = xTimerCreateStatic("bt_dl", pdMS_TO_TICKS(BT_XREC_SHUTDOWN_DEADLINE_MS), pdFALSE, NULL,
                                    deadline_cb, &s_dl_buf);
    s_guard_timer = xTimerCreateStatic("bt_guard", pdMS_TO_TICKS(BT_GUARD_PERIOD_MS), pdTRUE, NULL, guard_cb,
                                       &s_guard_buf);
    if (s_dl_timer == NULL || s_guard_timer == NULL) {
        return -1;
    }
    int err = bt_ctrl_rtos_init(&bt_rom_real, s_bd);   /* bt_ctrl + bt_dlv tasks, opcode-table validation */
    if (err != 0) {
        bt_xrec_plat_log_u32("BT bt_ctrl init failed", (uint32_t)err);
        return -1;
    }
    s_rec_task = xTaskCreateStatic(bt_recover_task, "bt_recover", BT_RECOVER_STACK_WORDS, NULL, BT_RECOVER_PRIO,
                                   s_rec_stack, &s_rec_tcb);
    return s_rec_task != NULL ? 0 : -1;
}

int bt_xrec_plat_stack_up_begin(exfat_vol_t *vol, int sd_mounted, const uint8_t bd_addr[6])
{
    s_vol = vol;
    s_sd = sd_mounted;
    memcpy(s_bd, bd_addr, 6u);
    s_app = xTaskGetCurrentTaskHandle();
    if (bt_port_init(store_flush_request) != 0) {       /* entropy, store, bt_alarm (idempotent) */
        return -1;
    }
    bonds_load();                                        /* before esp_bluedroid_init() (plan M2) */
    if (once_init() != 0) {
        return -2;
    }
    bt_ctrl_rtos_stats_t st;
    bt_ctrl_rtos_stats(&st);
    s_faults_base = st.faults;
    s_hci_to_base = bt_port_hci_command_timeouts();
    s_ctrl_hung = 0u;
    s_last_check = millis();
    mem_reset();
    (void)xTimerStart(s_guard_timer, 0);
    post_job(JOB_UP);
    return 0;
}

void bt_xrec_plat_stack_down_begin(void)
{
    taskENTER_CRITICAL();
    s_dl_fired = 0u;
    s_dl_armed = 1u;
    taskEXIT_CRITICAL();
    if (s_dl_timer == NULL || xTimerReset(s_dl_timer, 0) != pdPASS) {
        bt_xrec_plat_log("BT teardown deadline timer not armed (app-side deadline only)");
    }
    if (s_rec_task == NULL) {                            /* never initialised: nothing to tear down */
        taskENTER_CRITICAL();
        s_req_seq++;
        s_done_seq = s_req_seq;
        s_done_res = 0;
        taskEXIT_CRITICAL();
        return;
    }
    post_job(JOB_DOWN);
}

void bt_xrec_plat_hard_path(uint32_t reason)
{
    bt_ctrl_rtos_hard_stop();                            /* bt_stop + IRQ 26 masked (no ROM call) */
    if (bt_rom_real_busy()) {
        (void)bt_rom_real_silence();                     /* RWBTEN off + soft reset without the ROM */
    }
    recorder_poll();
    if (recorder_is_saving()) {
        (void)recorder_stop();                           /* finalise the recording (static buffers only) */
    }
    openfw_rtos_fatal(BT_FAULT_RECORD_CODE, reason, 0u); /* AON fault record + reboot; BT off next boot */
}

uint32_t bt_xrec_plat_poll(void)
{
    uint32_t now = millis();
    if (s_store_flush_req) {
        s_store_flush_req = 0u;
        bonds_write();
    } else if (s_bond_fails != 0u && s_bond_fails < BOND_RETRY_MAX && (int32_t)(now - s_bond_retry_at) >= 0) {
        bonds_write();               /* still dirty after a failed write: bounded retry, no write on every poll */
    }
    if (now - s_mem_sample_at >= BT_MEM_SAMPLE_MS) {
        s_mem_sample_at = now;
        mem_sample();
        if (now - s_mem_log_at >= BT_MEM_LOG_MS) {
            s_mem_log_at = now;
            mem_log("run");
        }
    }
    if (now - s_last_check < BT_FAULT_CHECK_MS) {
        return 0u;
    }
    s_last_check = now;
    bt_rom_real_diag_t rd;
    bt_rom_real_diag(&rd);
    s_irq_cache = rd.irq_total;
    if (s_ctrl_hung) {
        return 1u;
    }
    bt_ctrl_rtos_stats_t st;
    bt_ctrl_rtos_stats(&st);
    if (st.faults != s_faults_base) {
        bt_xrec_plat_log_u32("BT bt_ctrl fault, first code", st.first_fault);
        return 2u;
    }
    if (bt_port_hci_command_timeouts() != s_hci_to_base) {
        return 3u;
    }
    return 0u;
}

void bt_xrec_plat_session_end(void)
{
    if (s_guard_timer != NULL) {
        (void)xTimerStop(s_guard_timer, 0);
    }
    s_store_flush_req = 0u;
    bonds_write();                                       /* final flush of the bond store (retried once more) */
    s_bond_fails = 0u;
    mem_sample();                                        /* the Bluedroid tasks are gone: their last samples */
    mem_log("stop");
    s_vol = NULL;
    s_sd = 0;
}

void bt_xrec_plat_reboot(uint8_t mode)
{
    if (mode == 2u) {
        pmu_reboot_to_sd_ota();
    } else if (mode == 3u) {
        pmu_reboot_to_usb_bootloader();
    }
    system_reboot();
}
