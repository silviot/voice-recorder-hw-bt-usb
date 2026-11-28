/* Firmware platform for bt_xrec_bluedroid (BT=1 phone mode, plan M4): device callbacks for xrec_proto, the bond store
 * on SD, stack bring-up / teardown jobs in the bt_recover worker, the hard-stop deadline, the hard path.
 *
 * F: = src/bt_hci.c at 7c76b77 (deleted in plan step 9), M: = src/main.c (line numbers of that tree). The device
 * callbacks are the legacy ones (F:1733-1966) with the session's volume.
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
#include "openfw_crypto.h"
#include "openfw_exp.h"
#include "openfw_rtos.h"
#include "esp_err.h"
#include "esp_bt_main.h"
#include "esp_bluedroid_hci.h"
#include "esp_spp_api.h"
#include "bt_port.h"
#include "bt_store.h"
#include "bt_entropy.h"
#include "bt_entropy_mic.h"
#include "bt_p192.h"
#include "bt_rom_rng.h"
#include "bt_ctrl.h"
#include "bt_ctrl_rtos.h"
#include "bt_rom_real.h"
#include "bt_xrec_bluedroid.h"
#include "bt_phone_cfg_codec.h"
#include "bt_task_sup.h"
#include "bt_hp.h"
/* Bluedroid's osi thread API (osi/thread.h pulls in bt_common.h, whose bt_status_t clashes with openfw.h's). */
typedef struct osi_thread osi_thread_t;
bool osi_thread_post(osi_thread_t *thread, void (*func)(void *context), void *context, int queue_idx,
                     uint32_t timeout);

osi_thread_t *hci_get_current_thread(void);
osi_thread_t *btu_get_current_thread(void);
osi_thread_t *btc_get_current_thread(void);

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
#define HP_BOND_FILE            "bt_hp_bonds.bin"    /* headphones mode (docs/BT_A2DP_DESIGN.md §6.4) */
#define HP_BOND_TMP             ".bt_hp_bonds.tmp"
#define HP_CFG_FILE             "bt_hp_cfg.bin"      /* headphones settings: output mode, bt_vol, last headset */
#define HP_CFG_TMP              ".bt_hp_cfg.tmp"
#define BOND_RETRY_MS           1000u     /* a failed bond-file write is retried this often ... */
#define BOND_RETRY_MAX          8u        /* ... this many times per session (then at the next change / session end) */

/* ---- session ---- */
static exfat_vol_t *s_vol;
static int s_sd;
static uint8_t s_mode;                   /* BT_XREC_MODE_* of the running / next session */
static uint8_t s_store_mode = 0xFFu;     /* mode whose bond file the RAM store holds (0xFF: none loaded yet) */
static int (*s_profile_down)(void);      /* headphones: AVRCP + A2DP deinit, run by the down job */
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

/* ---- task liveness supervisor (bt_task_sup.h; plan M4 "Watchdog") ---- */
static bt_task_sup_t s_sup BT_HI_STATIC;
static uint8_t s_sup_ready;

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
static uint8_t s_hold_logged;            /* the bond-file hold (bt_store_held) was reported this session */
static uint32_t s_bond_retry_at;
static uint32_t s_last_check, s_faults_base, s_hci_to_base, s_irq_cache;

/* ---- entropy + ROM key material (plan M2; BT_ROM_INTERFACE §2.3) ---- */
#define ROM_RNG_TOPUP_MIN       64u       /* refill the ROM RNG pool once this much room is free */

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
static int assert_check(void);
void bt_xrec_plat_service(void) { (void)assert_check(); recorder_poll(); }
void bt_xrec_plat_wait(void) { (void)assert_check(); vTaskDelay(1); }
uint32_t bt_xrec_plat_irq_count(void) { return s_irq_cache; }

/* "BT ROM links lc=.. ld=.. rep=..", two hex digits per link 0..6 (bt_rom_real_links): tells a link the ROM still
 * holds (lc 5 / ld 1) from one it dropped without telling the host (rep 0). Always at a fault; in the bench image
 * also when an LC or LD state byte changed (not rep: its meaning is only inferred, and a busy field would log on
 * every poll). The release image does not log on change: the byte meanings come from static analysis only. */
static uint8_t s_links_last[3u * BT_ROM_REAL_LINKS];
static void rom_links_log(int force)
{
    uint8_t v[3u * BT_ROM_REAL_LINKS];
    bt_rom_real_links(&v[0], &v[BT_ROM_REAL_LINKS], &v[2u * BT_ROM_REAL_LINKS]);
    /* Silicon session 5: a free LC slot flaps 0x00 <-> 0x03 with every page/inquiry-scan window (several Hz), which
     * flooded the 8 KiB bench log (lost=60858). Treat 0x00 and 0x03 as one state for the change test. */
    int changed = force;
    for (uint32_t i = 0u; !changed && i < 2u * BT_ROM_REAL_LINKS; i++) {
        uint8_t a = v[i], b = s_links_last[i];
        if (i < BT_ROM_REAL_LINKS) {
            a = (a == 0x03u) ? 0x00u : a;
            b = (b == 0x03u) ? 0x00u : b;
        }
        changed = (a != b);
    }
    if (!changed) {
        return;
    }
    memcpy(s_links_last, v, sizeof(v));
    static const char k[] = "0123456789ABCDEF";
    static const char *const tag[3] = { "BT ROM links lc=", " ld=", " rep=" };
    for (uint32_t t = 0u; t < 3u; t++) {
        openfw_log_puts(tag[t]);
        for (uint32_t i = 0u; i < BT_ROM_REAL_LINKS; i++) {
            uint8_t b = v[t * BT_ROM_REAL_LINKS + i];
            openfw_log_putc(k[b >> 4]);
            openfw_log_putc(k[b & 0xFu]);
        }
    }
    openfw_log_putc('\n');
}

/* One bt_ctrl session log line (bt_ctrl_fault_format / bt_ctrl_stats_format); app task only. */
static char s_ctrl_line[BT_CTRL_STATS_LINE > BT_CTRL_FAULT_LINE ? BT_CTRL_STATS_LINE : BT_CTRL_FAULT_LINE];
static void log_ctrl_line(uint16_t (*fmt)(char *buf, uint16_t cap))
{
    (void)fmt(s_ctrl_line, (uint16_t)sizeof(s_ctrl_line));
    openfw_log_puts(s_ctrl_line);
    openfw_log_putc('\n');
}

_Static_assert(BT_XREC_NOTE_CONN == BT_CTRL_NOTE_CONN, "note kind seen by the session");
_Static_assert(BT_XREC_NOTE_DISC == BT_CTRL_NOTE_DISC, "note kind seen by the session");
_Static_assert(BT_XREC_NOTE_CONN_REQ == BT_CTRL_NOTE_CONN_REQ, "note kind seen by the session");
_Static_assert(BT_XREC_NOTE_SCAN_REFUSED == BT_CTRL_NOTE_SCAN_REFUSED, "note kind seen by the session");
_Static_assert(BT_XREC_NOTE_DISC_CMD == BT_CTRL_NOTE_DISC_CMD, "note kind seen by the session");
_Static_assert(BT_XREC_NOTE_NAME == BT_CTRL_NOTE_NAME, "note kind seen by the session");
_Static_assert(BT_XREC_NOTE_LINE == BT_CTRL_NOTE_LINE, "note line buffer");
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

int bt_xrec_plat_ctrl_name(uint8_t bda[6], char *name, uint16_t cap)
{
    return bt_ctrl_peer_name(bda, name, cap);
}

static volatile uint32_t s_park_reason;   /* F8 (below): nonzero = parked; the deferred hard path's reason */

int bt_xrec_plat_start_allowed(void)
{
    /* parked (F8): a stopped controller and BT tasks in an unknown state until the deferred reboot */
    return g_prev_fault_code != (OPENFW_FAULT_MAGIC_BASE | BT_FAULT_RECORD_CODE) && s_park_reason == 0u;
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

/* ============================================ entropy + ROM key material (app task) ============================= */
static int drbg_rng(void *ctx, void *out, size_t n)
{
    (void)ctx;
    return bt_entropy_get(out, n) == BT_ENT_OK ? 0 : -1;
}

/* Keep the ROM RNG hook's pool (bt_rom_rng.h) full of DRBG bytes. Without credited entropy bt_entropy_get() fails, the
 * pool stays empty and the security call sites count as starved (pairing refused by the gate). */
static void rom_rng_top_up(void)
{
    size_t room = bt_rom_rng_space();
    if (room < ROM_RNG_TOPUP_MIN) {
        return;
    }
    uint8_t buf[128];
    while (room >= ROM_RNG_TOPUP_MIN) {
        size_t k = room < sizeof(buf) ? room : sizeof(buf);
        if (bt_entropy_get(buf, k) != BT_ENT_OK) {
            break;                   /* fail closed: the hook counts starvation, the pairing gate refuses */
        }
        (void)bt_rom_rng_feed(buf, k);
        room = bt_rom_rng_space();
    }
    for (size_t i = 0; i < sizeof(buf); i++) {
        ((volatile uint8_t *)buf)[i] = 0u;
    }
}

/* Mic collection (seed, later reseed) and, once credited, the session's P-192 key pair for the ROM. Runs before the
 * bring-up job (radio off), so bt_rom_real's start() (HCI Reset inside esp_bluedroid_enable) installs the pair. This
 * is the only collection of a session: none runs while the radio is on (its effect on the mic LSBs is not measured),
 * so a collection the recorder or the REC switch prevented leaves the session without new credit (review findings). */
static void session_entropy_begin(const uint8_t bd_addr[6])
{
    bt_rom_real_stage_p192(NULL, NULL);
    bt_rom_rng_reset();              /* the ROM is stopped here: nothing can call the hook */
    uint32_t t = millis();
    bt_entropy_mix_uncredited(bd_addr, 6u);
    bt_entropy_mix_uncredited(&t, sizeof(t));
    int st = bt_entropy_mic_collect();
    if (st == BT_ENT_MIC_BUSY || st == BT_ENT_MIC_ABORTED) {
        bt_xrec_plat_log(st == BT_ENT_MIC_BUSY ? "BT entropy mic: skipped (recorder owns the ADC), no credit this session"
                                               : "BT entropy mic: skipped (REC switch on), no credit this session");
    }
    if (keys_read_rec_switch()) {
        /* the BT-mode loop leaves for the recorder at its first pass: no key generation in the way */
        bt_xrec_plat_log("BT P-192 key generation skipped: REC switch on");
        return;
    }
    if (!bt_entropy_ready()) {
        bt_xrec_plat_log("BT entropy not credited: SSP pairing window refused this session");
        return;
    }
    uint8_t d[BT_P192_BYTES], q[2u * BT_P192_BYTES];
    uint32_t t0 = millis();
    if (bt_p192_keygen(drbg_rng, NULL, d, q) == 0) {
#if defined(OPENFW_BT_KEYTEST_MISMATCH) && OPENFW_BT_KEYTEST_MISMATCH
        {   /* plan step 6b: advertise a valid public key that does NOT belong to d (Makefile KEYTEST_MISMATCH) */
            uint8_t d2[BT_P192_BYTES];
            if (bt_p192_keygen(drbg_rng, NULL, d2, q) == 0) {
                bt_xrec_plat_log("BT KEYTEST: staged public key belongs to another private key; pairing MUST fail");
            } else {
                bt_xrec_plat_log("BT KEYTEST: second keygen failed, pair is consistent (test invalid)");
            }
            for (size_t i = 0; i < sizeof(d2); i++) {
                ((volatile uint8_t *)d2)[i] = 0u;
            }
        }
#endif
        bt_rom_real_stage_p192(d, q);
        /* public value only: the top word of X, to compare across sessions and with the ROM's FA2B96D3... */
        uint32_t xhi = (uint32_t)q[20] | ((uint32_t)q[21] << 8) | ((uint32_t)q[22] << 16) | ((uint32_t)q[23] << 24);
        openfw_log_puts("BT P-192 session key generated (replaces the ROM's fixed key), ms ");
        openfw_log_u32(millis() - t0);
        openfw_log_puts(", public X ");
        openfw_log_hex(xhi);
        openfw_log_puts("...\n");
    } else {
        bt_xrec_plat_log("BT P-192 key generation failed: SSP pairing window refused this session");
    }
    for (size_t i = 0; i < sizeof(d); i++) {
        ((volatile uint8_t *)d)[i] = 0u;
    }
    rom_rng_top_up();
}

/* While the session runs: keep the ROM RNG hook's pool full (no mic collection with the radio on, see above). */
static void session_entropy_poll(uint32_t now)
{
    (void)now;
    rom_rng_top_up();
}

/* Plan M2.3 pairing gate: NULL when a new pairing may be offered, else the reason (logged by the session). */
const char *bt_xrec_plat_pairing_block(void)
{
    bt_ent_stats_t es;
    bt_entropy_get_stats(&es);
    if (es.health_failures != 0u) {
        return "entropy source failed its health test (until reboot)";
    }
    if (!bt_entropy_ready()) {
        return "entropy not credited";
    }
    bt_rom_real_sec_t sec;
    bt_rom_real_sec(&sec);
    if (!sec.key_installed) {
        return sec.key_unexpected ? "ROM P-192 key slots not as traced (key not replaced)"
                                  : "ROM P-192 key not replaced (re-enter BT mode)";
    }
    if (sec.rng.starved != 0u) {
        return "ROM RNG pool ran dry this session";
    }
    return NULL;
}

int bt_xrec_plat_rng_starved(void)
{
    return bt_rom_rng_starved() != 0u ? 1 : 0;
}

/* ============================================ files (app task) ================================================== */
static void wipe_mem(void *p, size_t n)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    for (size_t i = 0u; i < n; i++) {
        v[i] = 0u;
    }
}

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
    wipe_mem(blob, sizeof(blob));
}

int bt_xrec_plat_save_bound_key(const uint8_t key[32])
{
    int rc = 0;
    uint8_t blob[36];
    blob[0] = 'C';
    blob[1] = 'S';
    blob[2] = 'K';
    blob[3] = '5';
    memcpy(&blob[4], key, 32u);
    if (save_root_file_atomic(".cassini.tmp", "cassini.key", blob, (uint32_t)sizeof(blob)) != 0 && s_sd) {
        bt_xrec_plat_log("BT XREC key not saved (SD write failed): PAIR refused");
        rc = -1;
    }
    wipe_mem(blob, sizeof(blob));
    return rc;
}

void bt_xrec_plat_set_mode(uint8_t mode, int (*profile_down)(void))
{
    s_mode = mode;
    s_profile_down = profile_down;
}

/* The file of a mode's bonds. Loads use the mode of the session starting; writes use the mode whose bonds the RAM
 * store holds (s_store_mode), so a flush after a mode switch can never land in the other mode's file (review finding:
 * the headphones stop switched to phone mode before its final flush and overwrote bt_bonds.bin). */
static const char *bond_file(uint8_t mode) { return mode == BT_XREC_MODE_HP ? HP_BOND_FILE : BOND_FILE; }
static const char *bond_tmp(uint8_t mode) { return mode == BT_XREC_MODE_HP ? HP_BOND_TMP : BOND_TMP; }

static void bonds_write(void);

/* Bond store (plan M2 "Store"): loaded before esp_bluedroid_init(); written by the app when bt_store asks.
 * Review STEP10 (C10 / A2): unsaved RAM bonds are newer than the card's file, so a restart in the same mode keeps them
 * (and flushes) instead of reloading the older file; a mode switch first tries to flush the other mode's unsaved bonds
 * into their own file. A bond file that exists but whose bonds this session cannot take over (not readable, no memory,
 * rejected by bt_store_load, too large, or a config Bluedroid cannot parse: patch 0045) is held (bt_store_held) and
 * never replaced this session: btc_config_init() saves the (then empty) config at every start, which wrote an empty
 * store over the card's bonds (silicon 2026-11-25, bt_hp_bonds.bin 152 -> 43 B). The next start tries the file again. */
static void bonds_load(void)
{
    s_hold_logged = 0u;
    if (s_store_mode != s_mode) {
        /* the other mode's bonds must not be written into this mode's file (design §6.4) */
        if (s_store_mode != 0xFFu && bt_store_dirty()) {
            bonds_write();
            if (bt_store_dirty()) {
                bt_xrec_plat_log("BT bonds of the other mode not saved (SD write failed): dropped");
            }
        }
        bt_store_reset();                /* also clears the hold */
        s_store_mode = s_mode;
    } else if (bt_store_dirty() && !bt_store_held()) {
        bt_xrec_plat_log("BT bonds kept from RAM (unsaved changes are newer than the card)");
        s_store_flush_req = 1u;
        return;
    }
    if (!s_sd || s_vol == NULL) {
        return;                      /* no SD: RAM-only bonds for the session (whatever this boot already holds) */
    }
    char path[24];
    path[0] = '/';
    memcpy(&path[1], bond_file(s_mode), strlen(bond_file(s_mode)) + 1u);
    exfat_entry_t ent;
    if (exfat_find_path(s_vol, path, &ent) != 0 || ent.is_dir || ent.size == 0u) {
        bt_store_hold(0);            /* no file: the RAM store is the truth */
        return;
    }
    if (ent.size > BT_STORE_IMAGE_MAX) {
        bt_xrec_plat_log_u32("BT bond file too large (kept, not written this session), size", ent.size);
        bt_store_reset();
        bt_store_hold(1);
        return;
    }
    uint8_t *img = (uint8_t *)pvPortMalloc((size_t)ent.size);
    if (img == NULL) {
        bt_xrec_plat_log("BT bonds not loaded: no memory (bond file kept, not written this session)");
        bt_store_hold(1);
        return;
    }
    uint32_t nread = 0u;
    if (exfat_read_file(s_vol, &ent, 0u, img, (uint32_t)ent.size, &nread) == 0 && nread == ent.size) {
        esp_err_t r = bt_store_load(img, nread);     /* a rejected image is held by bt_store */
        if (r != ESP_OK) {
            bt_xrec_plat_log_u32("BT bond file rejected (kept, not written this session; store starts empty), err",
                                 (uint32_t)r);
        }
    } else {
        bt_xrec_plat_log("BT bond file unreadable (kept, not written this session)");
        bt_store_hold(1);
    }
    wipe_mem(img, (size_t)ent.size);
    vPortFree(img);
}

static void bonds_write(void)
{
    if (!bt_store_dirty() || !s_sd || s_vol == NULL || s_store_mode == 0xFFu) {
        return;
    }
    if (bt_store_held()) {
        if (!s_hold_logged) {
            s_hold_logged = 1u;
            bt_xrec_plat_log("BT bonds not saved: the card's bond file was not loaded this session (kept)");
        }
        return;
    }
    uint8_t *img = (uint8_t *)pvPortMalloc(BT_STORE_IMAGE_MAX);
    if (img == NULL) {
        s_store_flush_req = 1u;      /* retry on a later poll */
        return;
    }
    size_t n = bt_store_serialize(img, BT_STORE_IMAGE_MAX);
    if (n != 0u && save_root_file_atomic(bond_tmp(s_store_mode), bond_file(s_store_mode), img, (uint32_t)n) == 0) {
        bt_store_mark_saved();       /* only now: a failed write keeps the store dirty (review finding) */
        s_bond_fails = 0u;
    } else {
        bt_xrec_plat_log("BT bonds not saved (SD write failed, retrying)");
        if (s_bond_fails < BOND_RETRY_MAX) {
            s_bond_fails++;
        }
        s_bond_retry_at = millis() + BOND_RETRY_MS;
    }
    wipe_mem(img, BT_STORE_IMAGE_MAX);
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
        (void)exfat_stat_fs(s_vol, &out->free_mb, &out->total_mb);   /* cached free count (F4): no bitmap scan */
    }
    out->rec_idx   = recorder_get_rec_index();                         /* STAT field 9 (F7) */
    out->rec_bytes = recorder_get_flushed_wav_bytes();                 /* STAT field 11 (live tail) */
}

/* Sync while recording (openfw_xrec.h, docs/BT_SYNC_SPEED_AND_RECORDING.md F1/F6/F7). */
static uint8_t dev_rec_channels(void *user)
{
    (void)user;
    return recorder_is_saving() ? recorder_get_channels() : 0u;
}

static uint32_t dev_rec_pending(void *user)
{
    (void)user;
    return audio_ring_pending();
}

static uint32_t dev_rec_flushed_size(void *user)
{
    (void)user;
    return recorder_get_flushed_wav_bytes();
}

static uint32_t dev_now_us(void *user)
{
    (void)user;
    return (uint32_t)us_timer_now();
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
    if (keys_read_rec_switch()) {
        return -1;
    }
    return recorder_stop();
}

static int dev_file_busy(void *user, const char *name)
{
    (void)user;
    return recorder_is_recording_file(name);   /* DEL of the file being recorded: +XREC:ERR,BUSY (STEP10 A1) */
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
    .file_busy = dev_file_busy,
#if OPENFW_ENABLE_USB_EXP
    .exp_write = dev_exp_write,
    .exp_run = dev_exp_run,
#endif
    .read_log = dev_read_log,
    .reset_log = dev_reset_log,
    .audio_cfg = dev_audio_cfg,
    .rec_channels     = dev_rec_channels,
    .rec_pending      = dev_rec_pending,
    .rec_flushed_size = dev_rec_flushed_size,
    .live_entry = recorder_live_entry,
    .now_us           = dev_now_us,
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
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED && s_profile_down != NULL &&
        s_profile_down() != 0) {
        rc = 5;                      /* headphones: AVRCP/A2DP deinit reported an error (logged by the session) */
    }
    if (esp_bluedroid_get_status() == ESP_BLUEDROID_STATUS_ENABLED) {
        esp_spp_profile_status_t ps;
        memset(&ps, 0, sizeof(ps));
        if ((esp_spp_get_profile_status(&ps) == ESP_OK && ps.spp_inited) || bt_xrec_spp_deinit_needed()) {
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

/* ============================================ task supervisor ===================================================== */
/* App task only, and only while the stack is enabled (armed from the first platform poll, disarmed when the teardown
 * starts): the thread handles are valid then. Queue 0, no wait: a full queue is a refusal (counted as outstanding). */
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

/* src/wdt_timer.c wdt_feed() (BT=1 images; weak 0 there): the backstop of bt_task_sup.h. Any context. */
int bt_task_sup_feed_hold_now(void)
{
    return s_sup_ready ? bt_task_sup_hold(&s_sup, millis()) : 0;
}

static void sup_log(const char *when)
{
    bt_task_sup_stats_t st;
    bt_task_sup_stats(&s_sup, millis(), &st);
    openfw_log_puts("BT sup ");
    openfw_log_puts(when);
    openfw_log_puts(" stalls ");
    openfw_log_u32(st.stalls);
    for (unsigned i = 0u; i < BT_TASK_SUP_N; i++) {
        openfw_log_putc(' ');
        openfw_log_puts(bt_task_sup_name(i));
        openfw_log_puts(" n=");
        openfw_log_u32(st.answers[i]);
        openfw_log_puts(" max=");
        openfw_log_u32(st.max_ms[i]);
        openfw_log_puts(" ref=");
        openfw_log_u32(st.refused[i]);
        openfw_log_puts(" out=");
        openfw_log_u32(st.outstanding_ms[i]);
    }
    openfw_log_putc('\n');
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
    bt_task_sup_init(&s_sup, sup_post);
    s_sup_ready = 1u;
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
    session_entropy_begin(bd_addr);                      /* mic seed/reseed + ROM P-192 key, before start() */
    if (once_init() != 0) {
        return -2;
    }
    bt_ctrl_rtos_stats_t st;
    bt_ctrl_rtos_stats(&st);
    s_faults_base = st.faults;
    s_hci_to_base = bt_port_hci_command_timeouts();
    s_ctrl_hung = 0u;
    s_last_check = millis();
    memset(s_links_last, 0, sizeof(s_links_last));    /* first non-idle ROM link state of the session is logged */
    mem_reset();
    (void)xTimerStart(s_guard_timer, 0);
    post_job(JOB_UP);
    return 0;
}

void bt_xrec_plat_stack_down_begin(void)
{
    bt_task_sup_disarm(&s_sup);                          /* the teardown has its own deadline (bt_task_sup.h) */
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

static void hard_path_rec(uint32_t reason, uint32_t b) __attribute__((noreturn));
static void hard_path_rec(uint32_t reason, uint32_t b)
{
    bt_ctrl_rtos_hard_stop();                            /* bt_stop + IRQ 26 masked (no ROM call) */
    if (bt_rom_real_busy()) {
        (void)bt_rom_real_silence();                     /* RWBTEN off + soft reset without the ROM */
    }
    recorder_poll();
    if (recorder_is_saving()) {
        (void)recorder_stop();                           /* finalise the recording (static buffers only) */
    }
    openfw_rtos_fatal(BT_FAULT_RECORD_CODE, reason, b);  /* AON fault record + reboot; BT off next boot */
}

void bt_xrec_plat_hard_path(uint32_t reason)
{
    hard_path_rec(reason, 0u);
}

/* ============================================ F8: fault while recording ========================================= */
/* docs/BT_SYNC_SPEED_AND_RECORDING.md 2.5 "BT fault during REC": the hard path would finalise the recording and
 * reboot, i.e. end it. While a recording runs the sessions park instead (bt_xrec_bluedroid.h): RW-BT is silenced the
 * way the hard path does it (no ROM call, no RF work, no wait), the supervisors that could still reset the device are
 * disarmed, and the hard path itself (fault record 0xB1 + reboot, BT off on the next boot) waits for the end of the
 * recording. Outside a recording nothing changes. */

int bt_xrec_plat_rec_active(void) { return recorder_is_saving(); }

void bt_xrec_plat_park(uint32_t reason)
{
    if (reason == 0u) {
        reason = BT_XREC_FAULT_PLATFORM;
    }
    taskENTER_CRITICAL();
    int first = s_park_reason == 0u;
    if (first) {
        s_park_reason = reason;
    }
    taskEXIT_CRITICAL();
    bt_ctrl_rtos_hard_stop();                            /* bt_stop + IRQ 26 masked (no ROM call) */
    if (bt_rom_real_busy()) {
        (void)bt_rom_real_silence();                     /* RWBTEN off + soft reset without the ROM */
    }
    bt_task_sup_disarm(&s_sup);                          /* no stall fault and no WDT feed hold from here on */
    if (s_guard_timer != NULL) {
        (void)xTimerStop(s_guard_timer, 0);              /* bt_ctrl hang guard: the controller is stopped */
    }
    if (s_dl_timer != NULL) {
        (void)bt_xrec_plat_deadline_disarm();            /* a teardown deadline in flight: already silenced */
    }
    if (first) {
        openfw_log_puts("BT parked (fault while recording), reason ");
        openfw_log_u32(reason);
        openfw_log_puts(": RW-BT silenced, recording continues, hard path after REC\n");
    }
}

int bt_xrec_plat_park_poll(void)
{
    uint32_t reason = s_park_reason;
    if (reason == 0u) {
        return 0;
    }
    recorder_poll();
    if (recorder_is_saving()) {
        return 1;
    }
    openfw_log_puts("BT parked: recording ended, deferred hard path\n");
    hard_path_rec(reason, 0u);                           /* no recording left to finalise: fault record + reboot */
}

/* ============================================ assert() (review STEP10 A9) ====================================== */
/* BT images build Bluedroid with its assert()s live (no NDEBUG): picolibc's __assert_func printed and called abort()
 * -> _exit() -> reboot, without finalising a recording in progress. A failed assert in a task is now handed to the
 * session's app task, which takes the BT hard path (bt_stop, recorder finalised, fault record 0xB1 with reason
 * BT_XREC_FAULT_ASSERT and the line, reboot) at its next platform poll / service / wait. The asserting task stops
 * there, still holding whatever lock it held. Backstop: if the app task has not taken the hard path after
 * BT_ASSERT_BACKSTOP_MS (e.g. blocked on such a lock), the old reboot (0xF0, recording not finalised).
 * From an ISR, a critical section, before any BT session, or a second assert in the app task: the old reboot at once. */
#define BT_ASSERT_BACKSTOP_MS 1500u
static volatile uint8_t s_assert_pending;
static volatile uint32_t s_assert_line;

static void assert_fail(const char *file, int line) __attribute__((noreturn));
static void assert_fail(const char *file, int line)
{
    uint32_t ipsr, primask, basepri;
    __asm__ volatile ("mrs %0, ipsr" : "=r"(ipsr));
    __asm__ volatile ("mrs %0, primask" : "=r"(primask));
    __asm__ volatile ("mrs %0, basepri" : "=r"(basepri));
    TaskHandle_t app = s_app;
    if ((ipsr & 0x1FFu) != 0u || primask != 0u || basepri != 0u || app == NULL ||
        xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        openfw_rtos_fatal(0xF0u, (uint32_t)line, (uint32_t)(uintptr_t)file);
    }
    taskENTER_CRITICAL();
    uint8_t was = s_assert_pending;
    if (!was) {
        s_assert_line = (uint32_t)line;
        s_assert_pending = 1u;
    }
    taskEXIT_CRITICAL();
    if (xTaskGetCurrentTaskHandle() == app) {
        if (was) {
            openfw_rtos_fatal(0xF0u, (uint32_t)line, (uint32_t)(uintptr_t)file);   /* asserted inside the hard path */
        }
        openfw_log_puts("BT assert (app task), line ");
        openfw_log_u32((uint32_t)line);
        openfw_log_putc('\n');
        hard_path_rec(BT_XREC_FAULT_ASSERT, (uint32_t)line);
    }
    for (uint32_t t = 0u; t < BT_ASSERT_BACKSTOP_MS; t += 10u) {
        if (s_park_reason != 0u) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10u));
    }
    /* F8: the app parked BT for a recording: this task never runs again (it may hold Bluedroid locks; nothing calls
     * Bluedroid any more), the deferred hard path reboots after REC. */
    while (s_park_reason != 0u) {
        vTaskSuspend(NULL);
    }
    openfw_rtos_fatal(0xF0u, (uint32_t)line, (uint32_t)(uintptr_t)file);
}

/* App task: take the hard path for an assert another task reported. While recording (F8): park instead (BT silenced,
 * the asserting task stays blocked for good, the session parks at its next poll: bt_xrec_plat_poll() returns 5). */
static int assert_check(void)
{
    if (s_assert_pending && s_app != NULL && xTaskGetCurrentTaskHandle() == s_app) {
        if (s_park_reason != 0u) {
            return 1;                                    /* already parked: covered by the deferred hard path */
        }
        openfw_log_puts("BT assert in a BT task, line ");
        openfw_log_u32(s_assert_line);
        if (recorder_is_saving()) {
            openfw_log_puts(": recording, BT parked\n");
            bt_xrec_plat_park(BT_XREC_FAULT_ASSERT);
            return 1;
        }
        openfw_log_puts(": hard path\n");
        hard_path_rec(BT_XREC_FAULT_ASSERT, s_assert_line);
    }
    return 0;
}

/* picolibc's assert() targets (libc.a members of their own: these definitions replace them in BT images). */
void __assert_func(const char *file, int line, const char *func, const char *expr) __attribute__((noreturn));
void __assert_func(const char *file, int line, const char *func, const char *expr)
{
    (void)func;
    (void)expr;
    assert_fail(file, line);
}

void __assert_no_args(void) __attribute__((noreturn));
void __assert_no_args(void)
{
    assert_fail(NULL, 0);
}

uint32_t bt_xrec_plat_poll(void)
{
    if (assert_check() || s_park_reason != 0u) {
        return 5u;                                       /* F8: parked (assert while recording) */
    }
    uint32_t now = millis();
    session_entropy_poll(now);
    if (recorder_is_saving() || bt_hp_streaming() || audio_dac_is_busy()) {
        /* F6: no second card writer while recording or streaming audio. The request stays set (the store stays dirty in
         * RAM) and the bond file is written once playback pauses/stops or at session end. */
    } else if (s_store_flush_req) {
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
    if (s_sup_ready) {
        bt_task_sup_arm(&s_sup, now);                    /* no-op while armed: first poll with the stack enabled */
        uint32_t stalled = bt_task_sup_poll(&s_sup, now);
        if (stalled != 0u) {
            for (unsigned i = 0u; i < BT_TASK_SUP_N; i++) {
                if (stalled & (1u << i)) {
                    openfw_log_puts("BT task stalled: ");
                    openfw_log_puts(bt_task_sup_name(i));
                    openfw_log_putc('\n');
                }
            }
            sup_log("stall");
            return 4u;
        }
    }
#if defined(OPENFW_BT_BENCH_IMAGE) && OPENFW_BT_BENCH_IMAGE
    rom_links_log(0);
#endif
    bt_ctrl_rtos_stats_t st;
    bt_ctrl_rtos_stats(&st);
    if (st.faults != s_faults_base) {
        /* this session's first fault with its raise site and two key values (bt_ctrl.h BT_CTRL_SITE_*), read together
         * (st.first_fault is per boot), then bt_ctrl's silent-drop counters and the ROM's link states */
        log_ctrl_line(bt_ctrl_fault_format);
        log_ctrl_line(bt_ctrl_stats_format);
        rom_links_log(1);
        return 2u;
    }
    if (bt_port_hci_command_timeouts() != s_hci_to_base) {
        return 3u;
    }
    return 0u;
}

void bt_xrec_plat_session_end(void)
{
    bt_task_sup_disarm(&s_sup);
    if (s_guard_timer != NULL) {
        (void)xTimerStop(s_guard_timer, 0);
    }
    s_store_flush_req = 0u;
    bonds_write();                                       /* final flush of the bond store (retried once more) */
    if (bt_store_held() && !s_hold_logged) {
        bt_xrec_plat_log("BT bond file held this session (its bonds were not loaded; file kept)");
    }
    s_bond_fails = 0u;
    mem_sample();                                        /* the Bluedroid tasks are gone: their last samples */
    mem_log("stop");
    if (s_sup_ready) {
        sup_log("stop");
    }
    log_ctrl_line(bt_ctrl_stats_format);
    bt_rom_real_sec_t sec;
    bt_rom_real_sec(&sec);
    openfw_log_puts("BT ROM keys: installed ");
    openfw_log_u32(sec.key_installs);
    openfw_log_puts(" unexpected ");
    openfw_log_u32(sec.key_unexpected);
    openfw_log_puts(" rng drbg ");
    openfw_log_u32(sec.rng.drbg_calls);
    openfw_log_puts(" nonces ");
    openfw_log_u32(sec.rng.nonce_fills);
    openfw_log_puts(" legacy ");
    openfw_log_u32(sec.rng.legacy_calls);
    openfw_log_puts(" unknown ");
    openfw_log_u32(sec.rng.unknown_calls);
    openfw_log_puts(" starved ");
    openfw_log_u32(sec.rng.starved);
    openfw_log_putc('\n');
    bt_rom_real_stage_p192(NULL, NULL);                  /* the controller is stopped: staged key wiped */
    bt_rom_rng_reset();
    s_vol = NULL;
    s_sd = 0;
}

void bt_xrec_plat_reboot(uint8_t mode)
{
    recorder_poll();
    if (recorder_is_saving()) {
        (void)recorder_stop();       /* finalise any active recording before reboot / OTA / DFU */
    }
    if (mode == 2u) {
        pmu_reboot_to_sd_ota();
    } else if (mode == 3u) {
        pmu_reboot_to_usb_bootloader();
    }
    system_reboot();
}

/* ============================================ headphones settings file (bt_hp.h seam) ============================= */
#include "bt_hp.h"
#ifndef OPENFW_BT_BLUEDROID
#define OPENFW_BT_BLUEDROID 1        /* this file is BT=1 only; the bt_port library is built without the app's -D */
#endif
#include "audio_out.h"

/* SD:/bt_hp_cfg.bin. Read with the boot volume (main.c hands it over once, bt_hp_set_boot_volume; the session's s_vol
 * is only set while BT runs), so the settings screen can show the output mode with BT off. */
static exfat_vol_t *s_boot_vol;
static const int *s_boot_sd;

void bt_hp_set_boot_volume(exfat_vol_t *vol, const int *mounted)
{
    s_boot_vol = vol;
    s_boot_sd = mounted;
}

int bt_hp_plat_cfg_load(uint8_t *buf, uint32_t cap, uint32_t *len)
{
    if (s_boot_vol == NULL || s_boot_sd == NULL || !*s_boot_sd) {
        return -1;
    }
    exfat_entry_t ent;
    if (exfat_find_path(s_boot_vol, "/" HP_CFG_FILE, &ent) != 0 || ent.is_dir || ent.size == 0u || ent.size > cap) {
        return -1;
    }
    uint32_t nread = 0u;
    if (exfat_read_file(s_boot_vol, &ent, 0u, buf, (uint32_t)ent.size, &nread) != 0 || nread != ent.size) {
        return -1;
    }
    *len = nread;
    return 0;
}

int bt_hp_plat_cfg_save(const uint8_t *buf, uint32_t len)
{
    exfat_vol_t *v = s_vol;
    int sd = s_sd;
    if (v == NULL) {                 /* BT off: the boot volume */
        v = s_boot_vol;
        sd = (s_boot_vol != NULL && s_boot_sd != NULL) ? *s_boot_sd : 0;
    }
    exfat_vol_t *keep_v = s_vol;
    int keep_sd = s_sd;
    s_vol = v;
    s_sd = sd;
    int r = save_root_file_atomic(HP_CFG_TMP, HP_CFG_FILE, buf, len);
    s_vol = keep_v;
    s_sd = keep_sd;
    return r;
}

void bt_hp_plat_attach_sink(void *tx, void *sink, const void *link, int ring_is_dac_buf)
{
    aout_bt_attach(tx, sink, (const aout_bt_link_t *)link, ring_is_dac_buf);
}

int16_t *bt_hp_plat_borrow_dac_buf(uint32_t *bytes)
{
    return audio_dac_borrow_buf(bytes);
}

/* ============================================ phone sync & auto-off config (SD:/bt_phone_cfg.bin) ================ */
#define PHONE_CFG_FILE "bt_phone_cfg.bin"
#define PHONE_CFG_TMP  ".bt_phone_cfg.tmp"

void bt_phone_cfg_defaults(bt_phone_cfg_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->auto_sync = 1u;      /* automatic background sync enabled by default */
    cfg->auto_off_idx = 2u;   /* 5 minutes idle auto-power-off by default */
}

int bt_phone_cfg_load(bt_phone_cfg_t *cfg)
{
    bt_phone_cfg_defaults(cfg);
    if (cfg == NULL || s_boot_vol == NULL || s_boot_sd == NULL || !*s_boot_sd) {
        return -1;
    }
    exfat_entry_t ent;
    if (exfat_find_path(s_boot_vol, "/" PHONE_CFG_FILE, &ent) != 0 || ent.is_dir ||
        (ent.size != BT_PHONE_CFG_BYTES && ent.size != BT_PHONE_CFG_LEGACY_BYTES)) {
        return -1;
    }
    uint8_t b[BT_PHONE_CFG_BYTES];
    uint32_t nread = 0u;
    if (exfat_read_file(s_boot_vol, &ent, 0u, b, (uint32_t)ent.size, &nread) != 0 || nread != (uint32_t)ent.size) {
        return -1;
    }
    return bt_phone_cfg_unpack(b, nread, cfg);
}

int bt_phone_cfg_save(const bt_phone_cfg_t *cfg)
{
    if (cfg == NULL) {
        return -1;
    }
    uint8_t b[BT_PHONE_CFG_BYTES];
    bt_phone_cfg_pack(cfg, b);

    exfat_vol_t *v = s_vol;
    int sd = s_sd;
    if (v == NULL) {
        v = s_boot_vol;
        sd = (s_boot_vol != NULL && s_boot_sd != NULL) ? *s_boot_sd : 0;
    }
    exfat_vol_t *keep_v = s_vol;
    int keep_sd = s_sd;
    s_vol = v;
    s_sd = sd;
    int r = save_root_file_atomic(PHONE_CFG_TMP, PHONE_CFG_FILE, b, BT_PHONE_CFG_BYTES);
    s_vol = keep_v;
    s_sd = keep_sd;
    return r;
}

int bt_phone_has_bond_files(void)
{
    exfat_vol_t *v = s_vol != NULL ? s_vol : s_boot_vol;
    int sd = s_vol != NULL ? s_sd : ((s_boot_vol != NULL && s_boot_sd != NULL) ? *s_boot_sd : 0);
    if (!sd || v == NULL) {
        return 0;
    }
    exfat_entry_t ke, be;
    if (exfat_find_path(v, "/cassini.key", &ke) != 0 || ke.is_dir || ke.size != 36u) {
        return 0;
    }
    if (exfat_find_path(v, "/" BOND_FILE, &be) != 0 || be.is_dir || be.size <= 48u) {
        return 0;
    }
    return 1;
}
