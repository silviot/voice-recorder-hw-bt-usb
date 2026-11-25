/* Host platform for bt_port/bt_xrec_bluedroid.c (make test-bt-host, phone scenario): see host_xrec.h.
 *
 * Mirrors bt_port/bt_xrec_fw.c (the firmware platform) with the harness's pieces:
 *   - app task (OPENFW_PRIO_APP) owns the session: runs bt_xrec_poll() on every notification / 5 ms, executes the
 *     bt_xrec_* calls the test posts (bthx_call), and publishes a status snapshot;
 *   - bt_recover worker (priority 2): up job = bth_ctrl_open() (bt_ctrl_open + HCI driver attach) + esp_bluedroid_init
 *     + enable; down job = esp_spp_deinit (wait for ESP_SPP_UNINIT_EVT) + disable + bt_ctrl stop + deinit: the same
 *     order and checks as bt_xrec_fw.c job_up / job_down;
 *   - teardown deadline: a static FreeRTOS one-shot timer (daemon priority 9) latches like bt_xrec_fw.c deadline_cb;
 *     here it only records (the harness's bt_ctrl has no IRQ to mask). The hard path records its reason (on target it
 *     reboots); the scenario fails if either ever happens;
 *   - bond store: /bt_bonds.bin on the RAM exFAT volume, loaded before esp_bluedroid_init and written when bt_store
 *     requested a flush (host_glue.c counts the requests; polled here) and at session end: the firmware's
 *     bonds_load / bonds_write. XREC bound key: /cassini.key in the legacy CSK5 format, as the firmware;
 *   - device ops: STAT / RTC with fixed values (no PMU / RTC on the host), LS / GETS from the volume (xrec_proto);
 *   - identity: host_ctrl.c's controller address (bt_ctrl's BD_ADDR); entropy: bt_entropy (the TEST ONLY source of
 *     host_glue.c), so the XREC PIN and the CHAL nonce come from the DRBG as on target.
 * SPDX-License-Identifier: MIT */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "openfw_rtos.h"
#include "esp_err.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_spp_api.h"
#include "bt_ctrl.h"
#include "bt_entropy.h"
#include "bt_store.h"
#include "host_env.h"
#include "host_xrec.h"
/* Bluedroid's osi thread API (osi/thread.h pulls in bt_common.h, whose bt_status_t clashes with openfw.h's). */
typedef struct osi_thread osi_thread_t;
bool osi_thread_post(osi_thread_t *thread, void (*func)(void *context), void *context, int queue_idx,
                     uint32_t timeout);

osi_thread_t *hci_get_current_thread(void);
osi_thread_t *btu_get_current_thread(void);
osi_thread_t *btc_get_current_thread(void);

#define BTHX_RECOVER_PRIO 2u
/* as bt_xrec_fw.c: loads by the starting session's mode, writes by the mode whose bonds the RAM store holds */
#define BOND_FILE(m) ((m) == BT_XREC_MODE_HP ? "bt_hp_bonds.bin" : "bt_bonds.bin")
#define BOND_TMP(m) ((m) == BT_XREC_MODE_HP ? ".bt_hp_bonds.tmp" : ".bt_bonds.tmp")
static uint8_t s_mode;
static uint8_t s_store_mode = 0xFFu;
static int (*s_profile_down)(void);
static uint32_t s_profile_down_rc_last = 0xFFFFFFFFu;
#define HOST_TIMESTAMP 0x5B2A6000u    /* 2025-09-10 12:00:00 DOS timestamp: no RTC on the host */

static const uint8_t k_bd_addr[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0xC6 };   /* host_ctrl.c k_bd_addr */

static exfat_vol_t *s_card;             /* the RAM SD card (always mounted) */
static exfat_vol_t *s_vol;              /* the session's volume while one runs */
static int s_sd;
static TaskHandle_t s_app, s_rec;

/* test -> app task calls */
static volatile int s_req_op, s_req_res;
static volatile uint32_t s_req_arg, s_req_seq, s_req_done;
static const char *volatile s_rd_name;
static uint8_t *volatile s_rd_buf;
static volatile uint32_t s_rd_cap;
enum { BTHX_READ_FILE = 100, BTHX_FN = 101 };
static int (*volatile s_fn)(uint32_t);
static void (*volatile s_app_hook)(void);

static bthx_snap_t s_snap;

/* worker */
enum { JOB_NONE = 0, JOB_UP, JOB_DOWN };
static volatile uint32_t s_job_req, s_job_done;
static volatile uint8_t s_job_kind;
static volatile int s_job_res;

/* deadline / faults / telemetry */
static StaticTimer_t s_dl_buf;
static TimerHandle_t s_dl_timer;
static volatile uint8_t s_dl_armed, s_dl_fired;
static volatile uint32_t s_dl_fired_total, s_hard_reason, s_bond_writes, s_key_writes, s_reboot_mode;
static uint32_t s_faults_base, s_hci_to_base, s_flush_seen;
static uint8_t s_bonds_hold;                  /* as bt_xrec_fw.c: the bond file could not be read, never written */
static volatile uint8_t s_fail_bond_reads;    /* test knob: reading the mode's bond file fails (SD error) */
static volatile uint8_t s_fail_bond_writes;   /* test knob: every /bt_bonds.bin write fails (full / removed card) */
static volatile uint32_t s_bond_write_fails;

/* Bluedroid task supervisor: the firmware's (bt_xrec_fw.c sup_post), armed at the first platform poll, disarmed when
 * the teardown starts; a stall is platform fault 4. */
static bt_task_sup_t s_sup;
static volatile uint8_t s_sup_ready;
static volatile uint32_t s_sup_faults;
static volatile uint8_t s_block[BT_TASK_SUP_N];       /* test knob: a work item that keeps the thread busy */
static volatile uint8_t s_block_running[BT_TASK_SUP_N];

static int sup_post(unsigned idx, void (*fn)(void *ctx), void *ctx)
{
    osi_thread_t *t = idx == BT_TASK_SUP_HCI ? hci_get_current_thread()
                    : idx == BT_TASK_SUP_BTU ? btu_get_current_thread()
                    : btc_get_current_thread();
    if (t == NULL) {
        return 0;
    }
    return osi_thread_post(t, fn, ctx, 0, 0u) ? 1 : 0;
}

static void block_cb(void *ctx)
{
    unsigned idx = (unsigned)(uintptr_t)ctx;
    s_block_running[idx] = 1u;
    while (s_block[idx]) {
        vTaskDelay(1);                 /* the thread is stuck in this item (a lost wake-up, a wait that never ends) */
    }
    s_block_running[idx] = 0u;
}
static uint8_t s_bond_fails;                  /* as bt_xrec_fw.c: consecutive failures, bounded retry */
static uint32_t s_bond_retry_at;
#define BOND_RETRY_MS  1000u
#define BOND_RETRY_MAX 8u

/* ============================================ small helpers ===================================================== */
uint32_t bt_xrec_plat_now_ms(void) { return (uint32_t)(us_timer_now() / 1000u); }
/* The lock is a critical section, as on target (taskENTER_CRITICAL with interrupts masked): nothing that can block
 * (the entropy mutex behind the M2.3 gate) may run inside it. s_lock_depth lets the gate check that (review finding:
 * config_finish evaluated the gate under the lock). */
static volatile uint32_t s_lock_depth, s_gate_in_lock;
void bt_xrec_plat_lock(void)
{
    taskENTER_CRITICAL();
    s_lock_depth++;
}
void bt_xrec_plat_unlock(void)
{
    s_lock_depth--;
    taskEXIT_CRITICAL();
}
uint32_t bthx_gate_in_lock(void) { return s_gate_in_lock; }

void bt_xrec_plat_notify_app(void)
{
    if (s_app != NULL) {
        xTaskNotifyGive(s_app);
    }
}

void bt_xrec_plat_log(const char *msg) { bth_log("    [xrec] %s\n", msg); }
static volatile uint32_t s_ssp_logged = 0xFFFFFFFFu;
void bt_xrec_plat_log_u32(const char *msg, uint32_t v)
{
    if (strstr(msg, "numeric comparison accepted") != NULL) {
        s_ssp_logged = v;
    }
    bth_log("    [xrec] %s %u\n", msg, (unsigned)v);
}
uint32_t bthx_ssp_value_logged(void) { return s_ssp_logged; }
void *bt_xrec_plat_alloc(size_t n) { return pvPortMalloc(n); }
void bt_xrec_plat_free(void *p) { vPortFree(p); }
int bt_xrec_plat_start_allowed(void) { return s_hard_reason == 0u; }
void bt_xrec_plat_identity(uint8_t bd[6]) { memcpy(bd, k_bd_addr, 6u); }
void bt_xrec_plat_service(void) {}
void bt_xrec_plat_wait(void) { vTaskDelay(1); }
uint32_t bt_xrec_plat_irq_count(void) { return 0u; }
int bt_xrec_plat_ctrl_note(char *line, uint16_t cap, bt_xrec_note_t *info)
{
    bt_ctrl_note_t n;
    if (!bt_ctrl_note_pop(&n)) {
        return 0;
    }
    info->status = n.status;
    info->arg = n.reason;
    info->handle = n.handle;
    (void)bt_ctrl_note_format(&n, line, cap);
    return n.kind;
}
int bt_xrec_plat_ctrl_name(uint8_t bda[6], char *name, uint16_t cap) { return bt_ctrl_peer_name(bda, name, cap); }
void bt_xrec_plat_reboot(uint8_t mode) { s_reboot_mode = mode; }

/* Entropy as the session sees it on target (bt_xrec_fw.c). The DRBG itself is always seeded on the host (host_glue.c
 * TEST ONLY source, which Bluedroid's esp_random also uses); bthx_withhold_entropy() models a BT-mode entry whose mic
 * collection credited nothing: the XREC PIN and the CHAL nonce fail closed and the M2.3 gate refuses. */
static volatile uint8_t s_withhold;
static volatile uint8_t s_key_staged, s_key_installed;   /* bt_rom_real_stage_p192 / start() install, modelled */
static volatile uint32_t s_rng_starved;                   /* bt_rom_rng `starved`, modelled (bthx_rng_starve) */

void bthx_rng_starve(void) { __atomic_add_fetch(&s_rng_starved, 1u, __ATOMIC_SEQ_CST); }
int bt_xrec_plat_rng_starved(void) { return __atomic_load_n(&s_rng_starved, __ATOMIC_SEQ_CST) != 0u ? 1 : 0; }

static bool session_entropy_ready(void) { return !s_withhold && bt_entropy_ready(); }

int bt_xrec_plat_entropy(void *out, size_t n)
{
    if (!session_entropy_ready()) {
        return -1;
    }
    return bt_entropy_get(out, n) == BT_ENT_OK ? 0 : -1;
}

/* Plan M2.3 gate in bt_xrec_fw.c's order. The fake ROM has no P-192 key slots or RNG hook, so the ROM part is modelled
 * on the firmware's sequence: the key is staged at stack_up_begin only with credited entropy (session_entropy_begin)
 * and "installed" when esp_bluedroid_enable() returned (bt_rom_real start() runs inside its HCI Reset); stop wipes it.
 * A credit arriving later in the session (deferred mic collection) does not install it: SSP stays refused until the
 * next start, exactly as on target. The RNG-pool reason is the `starved` latch (bthx_rng_starve), reset at the next
 * stack_up_begin as bt_rom_rng_reset() does in session_entropy_begin. */
const char *bt_xrec_plat_pairing_block(void)
{
    if (s_lock_depth != 0u) {
        s_gate_in_lock++;            /* on target: xSemaphoreTake(entropy mutex) inside a critical section */
    }
    bt_ent_stats_t es;
    bt_entropy_get_stats(&es);
    if (es.health_failures != 0u) {
        return "entropy source failed its health test (until reboot)";
    }
    if (!session_entropy_ready()) {
        return "entropy not credited";
    }
    if (!s_key_installed) {
        return "ROM P-192 key not replaced (re-enter BT mode)";
    }
    if (bt_xrec_plat_rng_starved()) {
        return "ROM RNG pool ran dry this session";
    }
    return NULL;
}

/* ============================================ files (app task) ================================================== */
static void wipe_mem(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    for (size_t i = 0u; i < n; i++) {
        v[i] = 0u;
    }
}

static int save_root_file_atomic(const char *tmp_name, const char *final_name, const void *data, uint32_t len)
{
    if (!s_sd || s_vol == NULL) {
        return -1;
    }
    exfat_set_timestamp(s_vol, HOST_TIMESTAMP);
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

static int read_root_file(exfat_vol_t *vol, const char *name, uint8_t *buf, uint32_t cap)
{
    char path[48];
    path[0] = '/';
    strncpy(&path[1], name, sizeof(path) - 2u);
    path[sizeof(path) - 1u] = '\0';
    exfat_entry_t ent;
    if (vol == NULL || exfat_find_path(vol, path, &ent) != 0 || ent.is_dir || ent.size > cap) {
        return -1;
    }
    uint32_t nread = 0u;
    if (exfat_read_file(vol, &ent, 0u, buf, (uint32_t)ent.size, &nread) != 0 || nread != ent.size) {
        return -1;
    }
    return (int)nread;
}

/* As bt_xrec_fw.c: runs before stack_up_begin(), so the volume comes from the context (xrec_init()). */
void bt_xrec_plat_load_bound_key(xrec_ctx_t *ctx)
{
    uint8_t blob[36];
    if (read_root_file(ctx->vol, "cassini.key", blob, sizeof(blob)) == 36 && memcmp(blob, "CSK5", 4u) == 0) {
        xrec_set_bound_key(ctx, &blob[4]);
    }
    wipe_mem(blob, sizeof(blob));
}

int bt_xrec_plat_save_bound_key(const uint8_t key[32])
{
    int rc = 0;
    uint8_t blob[36];
    memcpy(blob, "CSK5", 4u);
    memcpy(&blob[4], key, 32u);
    if (save_root_file_atomic(".cassini.tmp", "cassini.key", blob, (uint32_t)sizeof(blob)) == 0) {
        s_key_writes++;
    } else {
        bt_xrec_plat_log("BT XREC key not saved (SD write failed): PAIR refused");
        rc = s_sd ? -1 : 0;
    }
    wipe_mem(blob, sizeof(blob));
    return rc;
}

void bt_xrec_plat_set_mode(uint8_t mode, int (*profile_down)(void))
{
    s_mode = mode;
    s_profile_down = profile_down;
}

static void bonds_write(void);

/* As bt_xrec_fw.c bonds_load (review STEP10 C10 / A2: unsaved RAM bonds kept over the older file, the other mode's
 * unsaved bonds flushed before the switch, an unreadable file never written over). */
static void bonds_load(void)
{
    if (s_store_mode != s_mode) {    /* as bt_xrec_fw.c (design §6.4) */
        if (s_store_mode != 0xFFu && bt_store_dirty()) {
            bonds_write();
            if (bt_store_dirty()) {
                bt_xrec_plat_log("BT bonds of the other mode not saved (SD write failed): dropped");
            }
        }
        bt_store_reset();
        s_store_mode = s_mode;
        s_bonds_hold = 0u;
    } else if (bt_store_dirty() && !s_bonds_hold) {
        bt_xrec_plat_log("BT bonds kept from RAM (unsaved changes are newer than the card)");
        return;                      /* the start's config save asks for the flush (bt_xrec_fw.c sets the request) */
    }
    if (!s_sd || s_vol == NULL) {
        return;
    }
    char path[24];
    snprintf(path, sizeof(path), "/%s", BOND_FILE(s_mode));
    exfat_entry_t ent;
    if (exfat_find_path(s_vol, path, &ent) != 0 || ent.is_dir || ent.size == 0u || ent.size > BT_STORE_IMAGE_MAX) {
        s_bonds_hold = 0u;
        return;
    }
    uint8_t *img = (uint8_t *)pvPortMalloc(BT_STORE_IMAGE_MAX);
    if (img == NULL) {
        bt_xrec_plat_log("BT bonds not loaded: no memory (bond file kept, not written this session)");
        s_bonds_hold = 1u;
        return;
    }
    int n = s_fail_bond_reads ? -1 : read_root_file(s_vol, BOND_FILE(s_mode), img, BT_STORE_IMAGE_MAX);
    if (n > 0) {
        s_bonds_hold = 0u;
        esp_err_t r = bt_store_load(img, (size_t)n);
        if (r != ESP_OK) {
            bt_xrec_plat_log_u32("BT bond file rejected (store starts empty), err", (uint32_t)r);
        }
    } else {
        bt_xrec_plat_log("BT bond file unreadable (kept, not written this session)");
        s_bonds_hold = 1u;
    }
    wipe_mem(img, BT_STORE_IMAGE_MAX);
    vPortFree(img);
}

static void bonds_write(void)
{
    if (!bt_store_dirty() || !s_sd || s_vol == NULL || s_store_mode == 0xFFu || s_bonds_hold) {
        return;
    }
    uint8_t *img = (uint8_t *)pvPortMalloc(BT_STORE_IMAGE_MAX);
    if (img == NULL) {
        return;                      /* retried on the next flush request / session end */
    }
    size_t n = bt_store_serialize(img, BT_STORE_IMAGE_MAX);
    if (n != 0u && !s_fail_bond_writes && save_root_file_atomic(BOND_TMP(s_store_mode), BOND_FILE(s_store_mode), img, (uint32_t)n) == 0) {
        bt_store_mark_saved();       /* as bt_xrec_fw.c: only after the write succeeded */
        s_bond_fails = 0u;
        s_bond_writes++;
    } else {
        s_bond_write_fails++;
        bt_xrec_plat_log("BT bonds not saved (SD write failed, retrying)");
        if (s_bond_fails < BOND_RETRY_MAX) {
            s_bond_fails++;
        }
        s_bond_retry_at = bt_xrec_plat_now_ms() + BOND_RETRY_MS;
    }
    wipe_mem(img, BT_STORE_IMAGE_MAX);
    vPortFree(img);
}

/* ============================================ device ops ======================================================== */
static void dev_get_stat(void *user, xrec_stat_t *out)
{
    (void)user;
    memset(out, 0, sizeof(*out));
    out->voltage_mv = 3900u;
    out->bat_pct = 69u;
    out->media_status = 0x10u;
    if (s_sd && s_vol != NULL) {
        (void)exfat_stat_fs(s_vol, &out->free_mb, &out->total_mb);
    }
}

static void dev_get_rtc(void *user, xrec_rtc_t *out)
{
    (void)user;
    out->year = 2026u;
    out->month = 11u;
    out->day = 24u;
    out->hour = 12u;
    out->minute = 0u;
    out->second = 0u;
    out->weekday = 2u;
}

static const xrec_ops_t k_dev_ops = { .get_stat = dev_get_stat, .get_rtc = dev_get_rtc };
const xrec_ops_t *bt_xrec_plat_device_ops(void) { return &k_dev_ops; }

/* ============================================ worker ============================================================ */
static volatile uint8_t s_hold_up;    /* test knob: the up job waits (the app task keeps running, state UP) */
static volatile uint8_t s_hold_btc;   /* test knob: the up job ends with BTC_TASK suspended (bthx_hold_btc) */
static TaskHandle_t volatile s_btc_held;
static volatile uint32_t s_down_spp_checked, s_down_spp_deinit;   /* down jobs past the SPP check / that deinit */
static volatile uint8_t s_force_profile_fail;   /* test knob: the next down job's profile teardown reports failure */

static int job_up(void)
{
    while (s_hold_up) {              /* on target the ROM bring-up takes long enough for the app task to run */
        vTaskDelay(1);
    }
    if (bth_ctrl_open() != 0) {      /* bt_ctrl_open + esp_bluedroid_attach_hci_driver */
        return 1;
    }
    if (esp_bluedroid_init() != ESP_OK) {
        return 3;
    }
    if (esp_bluedroid_enable() != ESP_OK) {
        return 4;
    }
    s_key_installed = s_key_staged;  /* firmware: bt_rom_real start() installed the staged P-192 pair (HCI Reset) */
    if (s_hold_btc) {                /* test knob: BTC_TASK queues, but does not run, what the session posts next */
        s_btc_held = xTaskGetHandle("BTC_TASK");
        if (s_btc_held != NULL) {
            vTaskSuspend(s_btc_held);
        }
    }
    return 0;
}

static uint32_t s_down_ms[4];          /* spp_deinit, disable, bt_ctrl stop, deinit durations of the last down job */

static int job_down(void)
{
    int rc = 0;
    uint64_t t = us_timer_now();
    memset(s_down_ms, 0, sizeof(s_down_ms));
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED && s_profile_down != NULL) {
        int pr = s_profile_down();
        if (s_force_profile_fail) {
            s_force_profile_fail = 0u;   /* test knob (one shot): as if the A2DP deinit had not confirmed */
            pr = 2;
        }
        s_profile_down_rc_last = (uint32_t)pr;
        if (pr != 0) {
            rc = 5;
        }
    }
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) {
        esp_spp_profile_status_t ps;
        memset(&ps, 0, sizeof(ps));
        int need = (esp_spp_get_profile_status(&ps) == ESP_OK && ps.spp_inited) || bt_xrec_spp_deinit_needed();
        s_down_spp_deinit += need ? 1u : 0u;
        s_down_spp_checked++;
        if (need) {
            if (esp_spp_deinit() == ESP_OK) {
                while (!bt_xrec_spp_uninit_seen()) {
                    vTaskDelay(1);
                }
            } else {
                rc = 1;
            }
        }
        s_down_ms[0] = (uint32_t)((us_timer_now() - t) / 1000u);
        t = us_timer_now();
        if (esp_bluedroid_disable() != ESP_OK) {
            rc = 2;
        }
        s_down_ms[1] = (uint32_t)((us_timer_now() - t) / 1000u);
    }
    t = us_timer_now();
    if (bth_ctrl_close(60000u) != BT_CTRL_STOP_OK) {   /* bt_ctrl stop once bt_dlv left the host */
        rc = 3;
    }
    s_down_ms[2] = (uint32_t)((us_timer_now() - t) / 1000u);
    t = us_timer_now();
    if (esp_bluedroid_get_status() != ESP_BLUEDROID_STATUS_UNINITIALIZED && esp_bluedroid_deinit() != ESP_OK) {
        rc = 4;
    }
    s_down_ms[3] = (uint32_t)((us_timer_now() - t) / 1000u);
    bth_log("    [xrec] down job: spp_deinit %u ms, disable %u ms, bt_ctrl stop %u ms, deinit %u ms, rc %d\n",
            (unsigned)s_down_ms[0], (unsigned)s_down_ms[1], (unsigned)s_down_ms[2], (unsigned)s_down_ms[3], rc);
    return rc;
}

static void bt_recover_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdFALSE, portMAX_DELAY);
        taskENTER_CRITICAL();
        uint32_t seq = s_job_req;
        uint8_t job = s_job_kind;
        taskEXIT_CRITICAL();
        if (seq == s_job_done) {
            continue;
        }
        int r = job == JOB_UP ? job_up() : job_down();
        taskENTER_CRITICAL();
        s_job_res = r;
        s_job_done = seq;
        taskEXIT_CRITICAL();
        bt_xrec_plat_notify_app();
    }
}

static void post_job(uint8_t job)
{
    taskENTER_CRITICAL();
    s_job_kind = job;
    s_job_req++;
    taskEXIT_CRITICAL();
    xTaskNotifyGive(s_rec);
}

int bt_xrec_plat_job_done(int *result)
{
    taskENTER_CRITICAL();
    int done = s_job_done == s_job_req;
    int r = s_job_res;
    taskEXIT_CRITICAL();
    if (done && result != NULL) {
        *result = r;
    }
    return done;
}

static void deadline_cb(TimerHandle_t t)
{
    (void)t;
    taskENTER_CRITICAL();
    if (s_dl_armed) {
        s_dl_armed = 0u;
        s_dl_fired = 1u;
        s_dl_fired_total++;
    }
    taskEXIT_CRITICAL();
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
int bt_xrec_plat_stack_up_begin(exfat_vol_t *vol, int sd_mounted, const uint8_t bd_addr[6])
{
    (void)bd_addr;
    s_vol = vol;
    s_sd = sd_mounted;
    bonds_load();                    /* before esp_bluedroid_init() (plan M2) */
    s_key_installed = 0u;            /* bt_xrec_fw.c session_entropy_begin: key staged only with credited entropy */
    s_key_staged = (uint8_t)session_entropy_ready();
    __atomic_store_n(&s_rng_starved, 0u, __ATOMIC_SEQ_CST);   /* bt_rom_rng_reset() in session_entropy_begin */
    s_faults_base = bth_ctrl_faults();
    s_hci_to_base = bth_hci_cmd_timeouts();
    s_flush_seen = bth_store_flush_requests();
    post_job(JOB_UP);
    return 0;
}

void bt_xrec_plat_stack_down_begin(void)
{
    bt_task_sup_disarm(&s_sup);
    s_key_installed = 0u;            /* the controller stops: bt_rom_real stop() wipes the session key */
    taskENTER_CRITICAL();
    s_dl_fired = 0u;
    s_dl_armed = 1u;
    taskEXIT_CRITICAL();
    (void)xTimerReset(s_dl_timer, 0);
    post_job(JOB_DOWN);
}

void bt_xrec_plat_hard_path(uint32_t reason)
{
    if (s_hard_reason == 0u) {
        s_hard_reason = reason;
    }
    bth_log("    [xrec] HARD PATH reason %u (target: fault record 0xB1 + reboot)\n", (unsigned)reason);
}

uint32_t bt_xrec_plat_poll(void)
{
    uint32_t fl = bth_store_flush_requests();
    if (fl != s_flush_seen) {        /* bt_store asked for a flush (nvs_commit from the BTC config save) */
        s_flush_seen = fl;
        bonds_write();
    } else if (s_bond_fails != 0u && s_bond_fails < BOND_RETRY_MAX &&
               (int32_t)(bt_xrec_plat_now_ms() - s_bond_retry_at) >= 0) {
        bonds_write();
    }
    if (bth_ctrl_faults() != s_faults_base) {
        char fl[BT_CTRL_FAULT_LINE], sl[BT_CTRL_STATS_LINE];   /* the firmware's lines (bt_xrec_fw.c) */
        (void)bt_ctrl_fault_format(fl, sizeof(fl));
        (void)bt_ctrl_stats_format(sl, sizeof(sl));
        bth_log("    [xrec] %s\n    [xrec] %s\n", fl, sl);
        return 2u;
    }
    if (bth_hci_cmd_timeouts() != s_hci_to_base) {
        return 3u;
    }
    if (s_sup_ready) {
        uint32_t now = bt_xrec_plat_now_ms();
        bt_task_sup_arm(&s_sup, now);
        uint32_t stalled = bt_task_sup_poll(&s_sup, now);
        if (stalled != 0u) {
            for (unsigned i = 0u; i < BT_TASK_SUP_N; i++) {
                if (stalled & (1u << i)) {
                    bth_log("    [xrec] BT task stalled: %s\n", bt_task_sup_name(i));
                }
            }
            s_sup_faults++;
            return 4u;
        }
    }
    return 0u;
}

void bthx_sup_limits(uint32_t period_ms, uint32_t stall_ms, uint32_t hold_ms)
{
    bt_task_sup_set_limits(&s_sup, period_ms, stall_ms, hold_ms);
}

void bthx_sup_stats(bt_task_sup_stats_t *out)
{
    bt_task_sup_stats(&s_sup, bt_xrec_plat_now_ms(), out);
}

int bthx_sup_hold(void)
{
    return bt_task_sup_hold(&s_sup, bt_xrec_plat_now_ms());
}

uint32_t bthx_sup_faults(void)
{
    return s_sup_faults;
}

int bthx_block_thread(unsigned idx, int on)
{
    if (idx >= BT_TASK_SUP_N) {
        return -1;
    }
    if (!on) {
        s_block[idx] = 0u;
        return 0;
    }
    s_block[idx] = 1u;
    return sup_post(idx, block_cb, (void *)(uintptr_t)idx) ? 0 : -1;
}

int bthx_block_running(unsigned idx)
{
    return idx < BT_TASK_SUP_N ? s_block_running[idx] : 0;
}

void bt_xrec_plat_session_end(void)
{
    bt_task_sup_disarm(&s_sup);
    bonds_write();                   /* final flush of the bond store */
    s_bond_fails = 0u;
    s_key_staged = s_key_installed = 0u;
    s_vol = NULL;
    s_sd = 0;
}

/* ============================================ app task ========================================================== */
static void snapshot(void)
{
    bthx_snap_t n;
    memset(&n, 0, sizeof(n));
    bt_xrec_get_status(&n.st);
    bt_xrec_get_debug(&n.d);
    bt_xrec_get_ui(&n.ui);
    n.bonds = esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED ? esp_bt_gap_get_bond_device_num() : -1;
    taskENTER_CRITICAL();
    n.polls = s_snap.polls + 1u;
    s_snap = n;
    taskEXIT_CRITICAL();
}

static void app_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
        if (s_req_seq != s_req_done) {
            int r = 0;
            switch (s_req_op) {
            case BTHX_START: r = bt_xrec_start(s_card, 1); break;
            case BTHX_STOP: bt_xrec_stop(); r = (int)bthx_hard_path_reason(); break;
            case BTHX_OPEN_PAIRING: bt_xrec_open_pairing(s_req_arg); break;
            case BTHX_SSP_ANSWER: r = bt_xrec_ssp_answer((int)s_req_arg); break;
            case BTHX_START_PAIR:            /* main.c with SD:/BTBENCH.RUN pair=1: bt_bench_begin() right after start */
                r = bt_xrec_start(s_card, 1);
                if (r == 0) {
                    bt_xrec_open_pairing(s_req_arg);
                }
                break;
            case BTHX_BENCH_AUTOCONFIRM: bt_xrec_set_bench_autoconfirm((int)s_req_arg); break;
            case BTHX_BENCH_XREC_PIN: {     /* arg 0..999999 -> "NNNNNN"; anything larger switches it off */
                char pin[8];
                uint32_t v = s_req_arg;
                for (int i = 5; i >= 0; i--) {
                    pin[i] = (char)('0' + v % 10u);
                    v /= 10u;
                }
                pin[6] = '\0';
                bt_xrec_set_bench_xrec_pin(s_req_arg <= 999999u ? pin : NULL);
                break;
            }
            case BTHX_READ_FILE: r = read_root_file(s_card, s_rd_name, s_rd_buf, s_rd_cap); break;
            case BTHX_FN: r = s_fn != NULL ? s_fn(s_req_arg) : -1; break;
            default: r = -1; break;
            }
            snapshot();              /* before "done": a test reading S() right after the call sees its effect */
            taskENTER_CRITICAL();
            s_req_res = r;
            s_req_done = s_req_seq;
            taskEXIT_CRITICAL();
        }
        bt_xrec_poll();
        if (s_app_hook != NULL) {
            s_app_hook();            /* headphones scenario: bt_hp_poll() and the fake player in the app task */
        }
        snapshot();
    }
}

void bthx_set_app_hook(void (*hook)(void)) { s_app_hook = hook; }
uint32_t bthx_profile_down_rc(void) { return s_profile_down_rc_last; }
exfat_vol_t *bthx_card(void) { return s_card; }

int bthx_call_fn(int (*fn)(uint32_t), uint32_t arg, uint32_t timeout_ms)
{
    s_fn = fn;
    return bthx_call(BTHX_FN, arg, timeout_ms);
}

uint32_t bthx_post_fn(int (*fn)(uint32_t), uint32_t arg)
{
    s_fn = fn;
    return bthx_post(BTHX_FN, arg);
}

int bthx_init(exfat_vol_t *vol)
{
    if (s_app != NULL) {
        return 0;
    }
    s_card = vol;
    s_dl_timer = xTimerCreateStatic("bt_dl", pdMS_TO_TICKS(BT_XREC_SHUTDOWN_DEADLINE_MS), pdFALSE, NULL, deadline_cb,
                                    &s_dl_buf);
    if (s_dl_timer == NULL) {
        return -1;
    }
    bt_task_sup_init(&s_sup, sup_post);
    s_sup_ready = 1u;
    if (xTaskCreate(bt_recover_task, "bt_recover", 8192u, NULL, BTHX_RECOVER_PRIO, &s_rec) != pdPASS ||
        xTaskCreate(app_task, "app", 16384u, NULL, OPENFW_PRIO_APP, &s_app) != pdPASS) {
        return -1;
    }
    return 0;
}

uint32_t bthx_post(int op, uint32_t arg)
{
    taskENTER_CRITICAL();
    s_req_op = op;
    s_req_arg = arg;
    s_req_seq++;
    uint32_t seq = s_req_seq;
    taskEXIT_CRITICAL();
    xTaskNotifyGive(s_app);
    return seq;
}

int bthx_done(uint32_t seq, int *result)
{
    taskENTER_CRITICAL();
    int done = s_req_done == seq;
    int r = s_req_res;
    taskEXIT_CRITICAL();
    if (done && result != NULL) {
        *result = r;
    }
    return done;
}

static int call(int op, uint32_t arg, uint32_t timeout_ms)
{
    uint32_t seq = bthx_post(op, arg);
    int r = -1000;
    for (uint32_t t = 0u; t < timeout_ms; t++) {
        if (bthx_done(seq, &r)) {
            return r;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return -1000;
}

int bthx_call(int op, uint32_t arg, uint32_t timeout_ms) { return call(op, arg, timeout_ms); }

int bthx_read_root_file(const char *name, uint8_t *buf, uint32_t cap)
{
    s_rd_name = name;
    s_rd_buf = buf;
    s_rd_cap = cap;
    return call(BTHX_READ_FILE, 0u, 5000u);
}

void bthx_snap(bthx_snap_t *out)
{
    taskENTER_CRITICAL();
    *out = s_snap;
    taskEXIT_CRITICAL();
}

uint32_t bthx_hard_path_reason(void) { return s_hard_reason; }
uint32_t bthx_deadline_fired(void) { return s_dl_fired_total; }
uint32_t bthx_bond_writes(void) { return s_bond_writes; }
void bthx_fail_bond_writes(int on) { s_fail_bond_writes = on ? 1u : 0u; }
void bthx_fail_bond_reads(int on) { s_fail_bond_reads = on ? 1u : 0u; }
uint32_t bthx_bond_write_fails(void) { return s_bond_write_fails; }
uint32_t bthx_key_writes(void) { return s_key_writes; }
uint32_t bthx_reboot_mode(void) { return s_reboot_mode; }
void bthx_withhold_entropy(int on) { s_withhold = on ? 1u : 0u; }
void bthx_hold_bringup(int on) { s_hold_up = on ? 1u : 0u; }
int bthx_hold_btc(int on)
{
    s_hold_btc = on ? 1u : 0u;
    if (!on && s_btc_held != NULL) {
        TaskHandle_t h = s_btc_held;
        s_btc_held = NULL;
        vTaskResume(h);
        return 1;
    }
    return 0;
}
int bthx_btc_held(void) { return s_btc_held != NULL; }
void bthx_force_profile_down_fail(void) { s_force_profile_fail = 1u; }
uint32_t bthx_down_spp_checked(void) { return s_down_spp_checked; }
uint32_t bthx_down_spp_deinit(void) { return s_down_spp_deinit; }
