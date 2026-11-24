/* hci_probe: plan M5 Exp 1 stages A and B (plan step 4) -- bt_ctrl against the real RW-BT ROM (bt_rom_real) with a
 * host-side driver stub instead of Bluedroid. Built only with `make BT=1 HCI_PROBE=1`.
 *
 * Runs only on request, once: the SD root file HCIPROBE.RUN (hci_probe_trigger.c) is deleted, and the deletion
 * verified, before anything touches the controller, so a probe that hangs or resets the device cannot run again on the
 * next boot unless the user creates the file again (a 32 s WDT reset is a PMU cold reset that clears AON_BACKUP, so an
 * AON marker alone cannot stop a reset loop). Its content picks the stage: empty or "A" = stage A, "B" = stage A then
 * stage B; anything else is consumed and refused ("HCIP OFF trig=bad_content"). Without the file the image behaves like
 * BT=1 and logs "HCIP OFF".
 * The probe runs in its own low-priority task (same priority as the app, which keeps feeding the WDT; the probe also
 * feeds it while it waits): stage A ~6 s on silicon, stage B ~33 s more; then BT stays off. While it runs, main.c refuses
 * recording and defers USB MSC (each Reset busy-waits ~0.3-0.5 s in RF calibration at bt_ctrl priority); legacy BT mode
 * stays refused for the rest of the boot if RW-BT may be left in an unknown state (hci_probe_active()).
 * Stage A:
 *   1. HCI Reset (0C03): bt_ctrl's real controller reset (RF calibration, ROM start, IRQ 26 on), then IRQ-26
 *      liveness (the controller must raise IRQ 26 within 500 ms).
 *   2. Read-only commands bt_ctrl answers locally (opcode table action SYN): 1005, 1001, 1009, 1002, 1004 p0/p1,
 *      0C14. The opcode table has no read-only FWD row whose ROM mapping the legacy firmware exercises on silicon,
 *      so no ke_msg is sent to an LM/LC task by this stage; every row that needs hardware/disassembly is logged as
 *      NOT_PROBED and never sent with a guessed KE id.
 *   3. 5 s idle window: IRQ-26 per-bit counts and coalescing, IRQ->bottom-half latency, ROM handler / run()
 *      durations, bt_ctrl CPU time.
 *   4. Clean restart x5: teardown (stop) + open + Reset (start) + liveness + Read BD_ADDR.
 * Stage B (on the session the last restart left up): the opcode-table V-B rows, i.e. the FWD writes whose ROM KE ids
 * and payload layouts the legacy glue uses on silicon (bt_hci.c F:882-904), with the parameters Bluedroid / the legacy
 * firmware send, each with a read-back where one exists:
 *   0C56 SSP mode 01           -> 1004 page 1 must report the SSP host bit (bt_ctrl state set on the ROM's s=00)
 *   0C01 Bluedroid's mask      -> hci_tl_env.evt_msk must read mask | mandatory (ROM handler + hook 0x00803BBC[7])
 *   0C24 CoD 04 04 20          -> no read exists (0C23 is not in the table)
 *   0C13 "Cassini-Probe-XXXX"  -> 0C14 must return the same 248 bytes (bt_ctrl name cache, opcode table §3.7)
 *   0C1A 03 (inquiry + page scan): 30 s discoverable window (IRQ-26 per-bit counts/rate, bt_ctrl CPU, events, ROM
 *        messages, g_bt_heap) -> 0C1A 00 -> 2 s window (the IRQ load must fall back to idle)
 *   0C01 zero mask -> evt_msk must read exactly the mandatory bits (distinguishes "hook ran" from the reset default,
 *        which Bluedroid's mask cannot), then Bluedroid's mask again. Last, so a failure there costs nothing else.
 * None of these rows has a terminal event: any event after the reply is logged (EVT) right after its CMD line.
 * Per command: 500 ms deadline, expected initial reply (from the table row), raw reply, latency (us); stage B adds
 * fwd= (bt_ctrl forwarded it to the ROM), romd= (ROM messages to TASK_HCI meanwhile) and hw= (g_bt_heap high-water).
 * Recovery: on a failure a controlled bt_ctrl teardown; in stage A a second failure stops the probe, in stage B the
 * first one does (a retry would repeat the same write); a teardown that does not finish in 1 s latches bt_stop + masks
 * IRQ 26 (plan M1 hard path). A controlled teardown is the ROM soft reset (stop()), so it also ends discoverability. The
 * hard paths (HARDSTOP, HANG) never complete stop(), so they also run bt_rom_real_silence(): stop()'s register sequence
 * (RWBTEN off + soft reset) without a ROM call; whether that silences the radio while a ROM call is stuck is unverified
 * on silicon, so the doc says to reboot at once after either.
 * Starvation: besides the 3 s hang guard (one bt_ctrl_run() stuck), stage B arms a CPU-share guard: bt_ctrl busy for
 * more than CPU_LIMIT_PM of CPU_SAMPLES consecutive 100 ms samples is treated as a hang (HANG why=cpu), so an IRQ-26
 * source that re-latches at once cannot starve the app (WDT feed) for 32 s and lose the whole run.
 *
 * Output: one line per result in g_openfw_log, prefix "HCIP " (tools/bt_probe_report.py parses them). The ring is
 * 4 KiB, so lines are compact and long replies are truncated (raw prefix + CRC-32 of the whole packet). Stage A + B
 * write more than the ring holds, so every HCIP line is also kept in SRAM_HI and, after END, the app task writes them
 * to SD:/HCIPROBE.LOG (hci_probe_poll(); USB MSC stays deferred until then).
 * SPDX-License-Identifier: MIT */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "openfw.h"
#include "openfw_rtos.h"
#include "sl6801_regs.h"
#include "sl6801_rom.h"
#include "bt_ctrl.h"
#include "bt_ctrl_cmds.h"
#include "bt_ctrl_rtos.h"
#include "bt_rom_real.h"
#include "hci_probe_trigger.h"

#define PROBE_STACK_WORDS   384u     /* 1.5 KiB: big buffers are static; watermark logged */
#define PROBE_PRIO          OPENFW_PRIO_APP
#define CMD_DEADLINE_US     500000u
#define RESET_DEADLINE_US   2000000u /* 0C03 = real reset: RF calibration (~0.3 s in QEMU) + ROM start, then the CC */
#define HANG_LIMIT_US       3000000u /* one bt_ctrl_run() longer than this = stuck in a ROM busy-wait */
#define GUARD_PERIOD_MS     100u
#define PROBE_FAULT_MARK    (OPENFW_FAULT_MAGIC_BASE | 0xB2u)   /* AON fault record while the probe runs */
/* QEMU full-path check only (make test-qemu-hciprobe-full): QEMU has no RW-BT model, so IRQ 26 never fires and the
 * ROM never answers a forwarded command; waiving the liveness check and stage B's reply timeouts lets the rest of the
 * sequence run there. Logged as waived=1; never set for a silicon image. */
#ifndef HCI_PROBE_WAIVE_LIVENESS
#define HCI_PROBE_WAIVE_LIVENESS 0
#endif
/* QEMU checks only (make test-qemu-hciprobe*): the QEMU SD image is blank, so run without the HCIPROBE.RUN request:
 * 1 = stage A, 2 = stage A + B. Logged as trig=autorun; never set for a silicon image. */
#ifndef HCI_PROBE_AUTORUN
#define HCI_PROBE_AUTORUN 0
#endif
#define LIVE_DEADLINE_US    500000u
#define TEARDOWN_DEADLINE_US 1000000u
#define IDLE_WINDOW_MS      5000u
#define RESTARTS            5u
#define MAX_FAILURES        2u
#define RX_SLOTS            3u
#define RX_BYTES            258u     /* largest H4 event: 1 + 2 + 255 */
#define HEX_FULL_MAX        32u      /* replies up to this many bytes are logged whole */
#define HEX_TRUNC           16u      /* longer ones: this prefix + CRC-32 */
#define EVT_LOG_MAX         4u       /* unsolicited events logged individually (per stage) */
/* stage B: discoverable (0C1A 03) window; the QEMU full-path image (no RW-BT, timings meaningless) uses 3 s */
#define SCAN_WINDOW_MS      (HCI_PROBE_WAIVE_LIVENESS ? 3000u : 30000u)
#define OFF_WINDOW_MS       2000u    /* stage B: after 0C1A 00 */
#define SETTLE_MS           50u      /* stage B: wait for unexpected events after each reply */
#define LOG_COPY_BYTES      8192u    /* stage A ~2.7 KB + stage B ~2.3 KB of HCIP lines + silicon-only EVT / WINBITS /
                                      * HANG lines; SD:/HCIPROBE.LOG (SRAM_HI .bss_hi had ~9 KB left at 6 KiB) */
#define CPU_LIMIT_PM        700u     /* stage B CPU-share guard: bt_ctrl above 70 % ... */
#define CPU_SAMPLES         10u      /* ... in 10 consecutive guard samples (GUARD_PERIOD_MS each, i.e. 1 s) */
#define PROBE_NAME_PREFIX   "Cassini-Probe-"
#define RTOS_STATIC __attribute__((section(".rtos_static"), aligned(8)))
/* Probe-only diagnostic buffers go to SRAM_HI (.bss_hi, zeroed at boot, plan §4 "~19 KB left"): the RTOS region keeps
 * its heap_5 margin for what the integrated image will also have (bt_ctrl rings, task stacks). */
#define PROBE_BSS   __attribute__((section(".bss_hi")))

static StaticTask_t s_tcb RTOS_STATIC;
static StackType_t  s_stack[PROBE_STACK_WORDS] RTOS_STATIC;
static StaticTimer_t s_guard_buf RTOS_STATIC;
static TaskHandle_t s_task;
static TimerHandle_t s_guard;
static volatile uint8_t s_active;           /* probe task running (recording refused, USB MSC deferred) */
static volatile uint8_t s_lockout;          /* bt_ctrl hung or hard-stopped: legacy BT mode refused until reboot */
static volatile uint8_t s_log_ready;        /* probe task finished: hci_probe_poll() saves the log copy */
static uint8_t s_decided;                   /* hci_probe_start() ran this boot */
static uint8_t s_stage;                     /* HCIP_STAGE_A / _B (from the request file) */
static const char *s_trig = "absent";

/* main.c hooks (declared there under OPENFW_HCI_PROBE) */
void hci_probe_start(exfat_vol_t *vol, int sd_mounted);
void hci_probe_poll(exfat_vol_t *vol, int sd_mounted);
int  hci_probe_active(void);
int  hci_probe_running(void);
static int hci_probe_active_after_end(void);

/* ---- host-side driver stub (bt_dlv context -> probe task) ---- */
typedef struct {
    uint16_t len;
    uint32_t t_us;
    uint8_t  data[RX_BYTES];
} rx_slot_t;
static rx_slot_t s_rx[RX_SLOTS] PROBE_BSS;
static volatile uint32_t s_rx_head, s_rx_tail, s_rx_drop, s_send_avail;

static inline uint32_t now32(void)
{
    return US_TIMER_LO;
}

static int stub_recv(uint8_t *data, uint16_t len)
{
    uint32_t t = now32();
    if (s_rx_head - s_rx_tail >= RX_SLOTS) {
        s_rx_drop++;                         /* the probe drains continuously; counted, never blocks bt_dlv */
        return 0;
    }
    rx_slot_t *s = &s_rx[s_rx_head % RX_SLOTS];
    uint16_t n = len < RX_BYTES ? len : (uint16_t)RX_BYTES;
    memcpy(s->data, data, n);
    s->len = len;
    s->t_us = t;
    __asm__ volatile ("dmb" ::: "memory");
    s_rx_head++;
    if (s_task != NULL) {
        xTaskNotifyGive(s_task);
    }
    return 0;
}

static void stub_send_avail(void)
{
    s_send_avail++;
}

static const bt_ctrl_host_cb_t k_host = { stub_send_avail, stub_recv };

/* ---- log line builder (whole lines pushed atomically; HCIP always starts a line) ---- */
static char s_line[384] PROBE_BSS;   /* longest: stage B WIN, ~360 B with every counter at 10 digits (lb_c truncates) */
static uint32_t s_ll;
static char s_copy[LOG_COPY_BYTES] PROBE_BSS;   /* every emitted line, for SD:/HCIPROBE.LOG */
static uint32_t s_copy_len, s_copy_lost;
static exfat_writer_t s_writer PROBE_BSS;

static void lb_c(char c)
{
    if (s_ll < sizeof(s_line) - 2u) {
        s_line[s_ll++] = c;
    }
}

static void lb_s(const char *s)
{
    while (*s != '\0') {
        lb_c(*s++);
    }
}

static void lb_u(uint32_t v)
{
    char b[10];
    int i = 0;
    do {
        b[i++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v != 0u);
    while (i > 0) {
        lb_c(b[--i]);
    }
}

static void lb_x(uint32_t v, int digits)
{
    static const char hx[] = "0123456789ABCDEF";
    for (int i = digits - 1; i >= 0; i--) {
        lb_c(hx[(v >> (4 * i)) & 0xFu]);
    }
}

static void lb_ku(const char *k, uint32_t v)
{
    lb_c(' ');
    lb_s(k);
    lb_c('=');
    lb_u(v);
}

static void lb_kx(const char *k, uint32_t v, int digits)
{
    lb_c(' ');
    lb_s(k);
    lb_s("=0x");
    lb_x(v, digits);
}

static void lb_ks(const char *k, const char *v)
{
    lb_c(' ');
    lb_s(k);
    lb_c('=');
    lb_s(v);
}

static void lb_begin(const char *tag)
{
    s_ll = 0u;
    lb_s("HCIP ");
    lb_s(tag);
}

static void lb_emit(void)
{
    s_line[s_ll++] = '\n';
    s_line[s_ll] = '\0';
    taskENTER_CRITICAL();
    uint32_t w = g_openfw_log.write_idx;
    if (w != 0u && g_openfw_log.buf[(w - 1u) & (OPENFW_LOG_SIZE - 1u)] != '\n') {
        openfw_log_putc('\n');               /* another task was mid-line: keep "HCIP " at a line start */
    }
    openfw_log_puts(s_line);
    taskEXIT_CRITICAL();
    if (s_copy_len + s_ll <= sizeof(s_copy)) {    /* s_ll counts the '\n' (not the NUL) */
        memcpy(&s_copy[s_copy_len], s_line, s_ll);
        s_copy_len += s_ll;
    } else {
        s_copy_lost++;                       /* whole lines only; counted in HCIP LOGFILE */
    }
}

static void lb_hex(const uint8_t *p, uint32_t n)
{
    for (uint32_t i = 0u; i < n; i++) {
        lb_x(p[i], 2);
    }
}

static uint32_t crc32(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (uint32_t i = 0u; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

static void lb_raw(const uint8_t *p, uint16_t len)
{
    uint32_t have = len < RX_BYTES ? len : RX_BYTES;
    uint32_t n = have <= HEX_FULL_MAX ? have : HEX_TRUNC;
    lb_s(" rx=");
    for (uint32_t i = 0u; i < n; i++) {
        lb_x(p[i], 2);
    }
    if (n < len) {
        lb_ku("trunc", n);
        lb_kx("crc", crc32(p, have), 8);
    }
}

static const char *exp_name(uint8_t e)
{
    static const char *const k[] = { "-", "V-A", "V-B", "V-C", "G-A", "G-B", "G-C" };
    return e < sizeof(k) / sizeof(k[0]) ? k[e] : "?";
}

/* ---- probe state ---- */
static uint8_t  s_bd[6];                 /* wire order (LSB first), as programmed into the ROM */
static uint32_t s_failures, s_seq, s_evt_logged, s_evt_unsolicited;
static uint8_t  s_hardstopped;
static uint8_t  s_tx[4u + 255u] PROBE_BSS;
static rx_slot_t s_reply PROBE_BSS;
static rx_slot_t s_pop PROBE_BSS;
static bt_ctrl_stats_t s_cs0 PROBE_BSS, s_cs1 PROBE_BSS;
static bt_rom_stats_t s_rs0 PROBE_BSS, s_rs1 PROBE_BSS;
static bt_rom_real_diag_t s_rd0 PROBE_BSS, s_rd1 PROBE_BSS;
static bt_ctrl_rtos_stats_t s_ts0 PROBE_BSS, s_ts1 PROBE_BSS;
static uint32_t s_t_begin;
static uint8_t  s_bx;                    /* stage B: CMD lines carry fwd= romd= hw= */
static uint32_t s_vfail;                 /* stage B read-backs that did not match */

enum { R_OK = 0, R_TIMEOUT, R_BAD, R_FAULT };
static const char *const k_res[] = { "OK", "TIMEOUT", "BAD", "FAULT" };

static void wait_ms(uint32_t ms)
{
    wdt_feed();
    vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1u);
    wdt_feed();
}

static uint32_t faults_now(void)
{
    bt_ctrl_rtos_stats(&s_ts1);
    return s_ts1.faults;
}

static bool rx_pop(rx_slot_t *out)
{
    if (s_rx_tail == s_rx_head) {
        return false;
    }
    __asm__ volatile ("dmb" ::: "memory");
    *out = s_rx[s_rx_tail % RX_SLOTS];
    s_rx_tail++;
    return true;
}

static void log_unsolicited(const rx_slot_t *r)
{
    s_evt_unsolicited++;
    if (s_evt_logged >= EVT_LOG_MAX) {
        return;
    }
    s_evt_logged++;
    lb_begin("EVT");
    lb_ku("len", r->len);
    lb_raw(r->data, r->len);
    lb_emit();
}

/* Send one command through bt_ctrl and wait for its initial reply. quiet: no CMD line (restart cycles). */
static int do_cmd(uint16_t op, const uint8_t *prm, uint8_t plen, bool quiet, uint32_t *lat_out)
{
    const bt_ctrl_cmd_row_t *row = bt_ctrl_cmds_lookup(op);
    uint8_t want = row != NULL ? row->rsp : BTC_CS;
    uint8_t want_plen = row != NULL ? row->plen : 4u;
    uint32_t deadline = op == BT_CTRL_OP_RESET ? RESET_DEADLINE_US : CMD_DEADLINE_US;
    uint32_t f0 = faults_now();
    uint32_t n = ++s_seq;
    uint32_t fwd0 = 0u, rom0 = 0u;
    if (s_bx) {
        bt_ctrl_get_stats(&s_cs1);
        fwd0 = s_cs1.cmd_fwd;
        rom0 = s_cs1.rom_msgs;
    }
    int res = R_TIMEOUT;
    uint32_t lat = deadline;
    bool got = false;

    while (rx_pop(&s_pop)) {                 /* late replies of an earlier (timed-out) command never match this one */
        log_unsolicited(&s_pop);
    }
    uint32_t t0 = now32();
    while (!bt_ctrl_check_send_available()) {
        if (now32() - t0 >= deadline) {
            break;
        }
        wait_ms(1u);
    }
    if (bt_ctrl_check_send_available()) {
        s_tx[0] = BT_H4_CMD;
        s_tx[1] = (uint8_t)(op & 0xFFu);
        s_tx[2] = (uint8_t)(op >> 8);
        s_tx[3] = plen;
        if (plen != 0u) {
            memcpy(&s_tx[4], prm, plen);
        }
        uint32_t t_send = now32();
        bt_ctrl_send(s_tx, (uint16_t)(4u + plen));
        while (!got && now32() - t_send < deadline) {
            while (!got && rx_pop(&s_pop)) {
                const uint8_t *d = s_pop.data;
                bool reply = s_pop.len >= 6u && d[0] == BT_H4_EVT &&
                             ((d[1] == BT_HCI_EVT_CC && (uint16_t)(d[4] | (d[5] << 8)) == op) ||
                              (d[1] == BT_HCI_EVT_CS && s_pop.len >= 7u && (uint16_t)(d[5] | (d[6] << 8)) == op));
                if (reply) {
                    s_reply = s_pop;
                    got = true;
                } else {
                    log_unsolicited(&s_pop);
                }
            }
            if (!got) {
                wdt_feed();
                (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5));
            }
        }
        if (got) {
            lat = s_reply.t_us - t_send;
            const uint8_t *d = s_reply.data;
            bool is_cc = d[1] == BT_HCI_EVT_CC;
            bool shape = (want == BTC_CC) ? (is_cc && d[2] == want_plen && d[3] == 1u &&
                                              s_reply.len == 3u + (uint32_t)want_plen)
                                           : (!is_cc && d[2] == 4u && d[4] == 1u && s_reply.len == 7u);
            uint8_t st = is_cc ? d[6] : d[3];
            res = (shape && st == BT_HCI_OK) ? R_OK : R_BAD;
        }
    }
    if (res == R_OK && faults_now() != f0) {
        res = R_FAULT;
    }
    bool waived = HCI_PROBE_WAIVE_LIVENESS && s_bx && res == R_TIMEOUT;   /* QEMU: no ROM reply ever comes */
    if (!quiet || res != R_OK) {
        lb_begin("CMD");
        lb_ku("n", n);
        lb_c(' ');
        lb_s("op=");
        lb_x(op, 4);
        if (plen != 0u) {
            lb_s(" p=");
            lb_hex(prm, plen <= 8u ? plen : 1u);    /* whole parameter up to 8 bytes (stage A: 1004's page) */
        }
        if (plen > 8u) {
            lb_ku("pl", plen);
        }
        lb_ks("want", want == BTC_CC ? "CC" : "CS");
        if (got) {
            const uint8_t *d = s_reply.data;
            lb_ks("got", d[1] == BT_HCI_EVT_CC ? "CC" : "CS");
            lb_ku("plen", d[2]);
            lb_c(' ');
            lb_s("st=");
            lb_x(d[1] == BT_HCI_EVT_CC ? d[6] : d[3], 2);
        } else {
            lb_ks("got", "NONE");
        }
        lb_ku("lat", lat);
        lb_ks("res", k_res[res]);
        if (got && op == 0x1009u && s_reply.len >= 13u) {
            lb_ks("bd", memcmp(&s_reply.data[7], s_bd, 6u) == 0 ? "match" : "MISMATCH");
        }
        if (s_bx) {                          /* forwarded to the ROM? ROM messages since the send; heap high-water */
            bt_ctrl_get_stats(&s_cs1);
            bt_rom_real_diag(&s_rd1);
            lb_ku("fwd", s_cs1.cmd_fwd - fwd0);
            lb_ku("romd", s_cs1.rom_msgs - rom0);
            lb_ku("hw", s_rd1.heap_hw);
            bt_ctrl_rtos_stats(&s_ts1);
            lb_ku("ctrl_free", s_ts1.ctrl_stack_free_words * 4u);   /* stack trend per ROM-forwarded command */
        }
        if (waived) {
            lb_ku("waived", 1u);
        }
        if (got) {
            lb_raw(s_reply.data, s_reply.len);
        }
        lb_emit();
    }
    if (waived) {
        res = R_OK;
        if (!got) {
            memset(&s_reply, 0, sizeof(s_reply));   /* read-backs then report MISMATCH, never stale data */
        }
    }
    if (lat_out != NULL) {
        *lat_out = lat;
    }
    return res;
}

/* IRQ 26 liveness after a Reset: the running controller raises IRQ 26 (legacy idle: ~1.6 kHz, BT_HW_RESULTS). */
static int check_live(uint32_t irq_before, bool quiet, uint32_t *irqs)
{
    uint32_t t0 = now32();
    bt_rom_real_diag(&s_rd1);
    while (s_rd1.irq_total == irq_before && now32() - t0 < LIVE_DEADLINE_US) {
        wait_ms(5u);
        bt_rom_real_diag(&s_rd1);
    }
    uint32_t dt = now32() - t0;
    int res = s_rd1.irq_total != irq_before ? R_OK : R_TIMEOUT;
    bool waived = HCI_PROBE_WAIVE_LIVENESS && res != R_OK;
    if (irqs != NULL) {
        *irqs = s_rd1.irq_total - irq_before;
    }
    if (!quiet || res != R_OK) {
        lb_begin(res == R_OK || waived ? "LIVE" : "TIMEOUT");
        lb_ks("what", "irq26");
        lb_ku("irq", s_rd1.irq_total - irq_before);
        lb_ku("wait", dt);
        lb_ku("deadline", LIVE_DEADLINE_US);
        if (waived) {
            lb_ku("waived", 1u);
        }
        lb_emit();
    }
    if (waived) {
        res = R_OK;
    }
    return res;
}

/* Controlled teardown (plan M1 order inside bt_ctrl). Returns 0 ok, 1 stop reported an error, -1 hard-stopped. */
static int teardown(const char *why, bool quiet)
{
    bt_ctrl_shutdown_begin();
    uint32_t t0 = now32();
    int st = bt_ctrl_shutdown_status();
    while (st == BT_CTRL_STOP_PENDING && now32() - t0 < TEARDOWN_DEADLINE_US) {
        wait_ms(2u);
        st = bt_ctrl_shutdown_status();
    }
    uint32_t dt = now32() - t0;
    static const char *const k_stop[] = { "OK", "ERR", "PARKED" };
    if (!quiet || st != BT_CTRL_STOP_OK) {
        lb_begin("TEARDOWN");
        lb_ks("why", why);
        lb_ks("st", st == BT_CTRL_STOP_PENDING ? "PENDING" : k_stop[st < 0 || st > 2 ? 1 : st]);
        lb_ku("us", dt);
        lb_emit();
    }
    if (st == BT_CTRL_STOP_PENDING) {
        bt_ctrl_rtos_hard_stop();            /* bt_ctrl parks before its next ROM call; IRQ 26 masked */
        s_hardstopped = 1u;
        s_lockout = 1u;
        uint32_t cntl = bt_rom_real_silence();   /* stop() never ran: RWBTEN off + soft reset without the ROM */
        lb_begin("HARDSTOP");
        lb_ks("why", "teardown_deadline");
        lb_kx("cntl", cntl, 8);
        lb_emit();
        return -1;
    }
    return st == BT_CTRL_STOP_OK ? 0 : 1;
}

/* open + Reset + liveness. On failure: teardown and, below MAX_FAILURES, one more attempt. */
static bool session_up(bool quiet, uint32_t *reset_lat, uint32_t *irqs);

static bool fail(const char *why)
{
    s_failures++;
    lb_begin("FAIL");
    lb_ku("n", s_failures);
    lb_ks("why", why);
    lb_emit();
    if (teardown(why, false) < 0) {
        s_failures = MAX_FAILURES;           /* hard-stopped: nothing more may touch the controller */
        return false;
    }
    return s_failures < MAX_FAILURES;
}

static bool session_up(bool quiet, uint32_t *reset_lat, uint32_t *irqs)
{
    for (;;) {
        if (bt_ctrl_open() != 0) {
            if (!fail("open")) {
                return false;
            }
            continue;
        }
        bt_rom_real_diag(&s_rd0);
        if (do_cmd(BT_CTRL_OP_RESET, NULL, 0u, quiet, reset_lat) != R_OK) {
            if (!fail("reset")) {
                return false;
            }
            continue;
        }
        if (check_live(s_rd0.irq_total, quiet, irqs) != R_OK) {
            if (!fail("irq26")) {
                return false;
            }
            continue;
        }
        return true;
    }
}

/* Every opcode-table row that needs hardware or disassembly and is not exercised here (never sent). */
static void log_not_probed(void)
{
    static const uint16_t k_probed[] = { 0x0C03u, 0x1005u, 0x1001u, 0x1009u, 0x1002u, 0x1004u, 0x0C14u };
    static const struct { uint8_t exp; const char *what; } k_groups[] = {
        { BTC_EXP_G_A, "needs disassembly" }, { BTC_EXP_G_B, "needs disassembly" },
        { BTC_EXP_G_C, "needs disassembly" }, { BTC_EXP_V_A, "needs disassembly" },
        { BTC_EXP_V_B, "needs stage B" },     { BTC_EXP_V_C, "needs stage C" },
    };
    uint32_t nrows = 0u;
    const bt_ctrl_cmd_row_t *rows = bt_ctrl_cmds_table(&nrows);
    for (uint32_t g = 0u; g < sizeof(k_groups) / sizeof(k_groups[0]); g++) {
        if (k_groups[g].exp == BTC_EXP_V_B && s_stage == HCIP_STAGE_B) {
            continue;                        /* probed by this run's stage B */
        }
        uint32_t i = 0u;
        while (i < nrows) {
            uint32_t on_line = 0u;
            for (; i < nrows && on_line < 24u; i++) {
                bool probed = false;
                for (uint32_t k = 0u; k < sizeof(k_probed) / sizeof(k_probed[0]); k++) {
                    probed = probed || rows[i].op == k_probed[k];
                }
                if (probed || rows[i].exp != k_groups[g].exp) {
                    continue;
                }
                if (on_line == 0u) {
                    lb_begin("NOT_PROBED ");
                    lb_s(k_groups[g].what);
                    lb_ks("exp", exp_name(k_groups[g].exp));
                    lb_s(" ops=");
                } else {
                    lb_c(',');
                }
                lb_x(rows[i].op, 4);
                on_line++;
            }
            if (on_line != 0u) {
                lb_emit();
            }
        }
    }
    /* Probed rows whose Exp 1 verification needs a ROM path this stage does not drive. */
    lb_begin("NOT_PROBED needs disassembly");
    lb_ks("xcheck", "1009:E05D->E0C0,1001:LMP_version_res");
    lb_emit();
}

/* Local identity for the probe (legacy F:1985-2003 eFuse branch; plan M1 forbids the timer fallback). */
static const char *probe_bd_addr(uint8_t bd[6])
{
    rom_rst_release(0x58u);
    rom_clk_enable(0x1Bu);
    bool efuse = (EFUSE_WORD(3) & (1u << 31)) != 0u;
    uint32_t ef0 = efuse ? EFUSE_WORD(0) : 0x0001u;
    uint32_t ef1 = efuse ? EFUSE_WORD(1) : 0x00u;
    bd[0] = (uint8_t)(ef0 & 0xFFu);
    bd[1] = (uint8_t)((ef0 >> 8) & 0xFFu);
    bd[2] = (uint8_t)(ef1 & 0xFFu);
    bd[3] = 0x55u;
    bd[4] = 0xCAu;
    bd[5] = 0x02u;                           /* locally administered, unicast */
    return efuse ? "efuse" : "fixed";
}

static void snapshot(bt_ctrl_stats_t *cs, bt_rom_stats_t *rs, bt_rom_real_diag_t *rd, bt_ctrl_rtos_stats_t *ts)
{
    bt_ctrl_get_stats(cs);
    bt_rom_real.stats(rs);
    bt_rom_real_diag(rd);
    bt_ctrl_rtos_stats(ts);
}

static void log_nvic(void)
{
    bt_rom_real_diag(&s_rd1);
    uint32_t bits = 0u;
    for (uint32_t m = s_rd1.ipr_impl_mask; m & 0x80u; m = (m << 1) & 0xFFu) {
        bits++;
    }
    lb_begin("NVIC");
    lb_ku("irq", IRQ_BT);
    lb_kx("impl", s_rd1.ipr_impl_mask, 2);
    lb_ku("bits", bits);
    lb_kx("set", s_rd1.ipr_set, 2);
    lb_kx("rb", s_rd1.ipr_readback, 2);
    lb_kx("maxsys", configMAX_SYSCALL_INTERRUPT_PRIORITY, 2);
    lb_kx("shpr3", s_rd1.shpr3, 8);
    lb_emit();
}

static void idle_window(void)
{
    bt_rom_real_diag_reset_max();
    bt_ctrl_rtos_reset_max();
    snapshot(&s_cs0, &s_rs0, &s_rd0, &s_ts0);
    uint32_t t0 = now32();
    for (uint32_t ms = 0u; ms < IDLE_WINDOW_MS; ms += 100u) {
        wait_ms(100u);
    }
    uint32_t win = now32() - t0;
    snapshot(&s_cs1, &s_rs1, &s_rd1, &s_ts1);

    lb_begin("IDLE");
    lb_ku("us", win);
    lb_ku("irq", s_rd1.irq_total - s_rd0.irq_total);
    lb_ku("spur", s_rd1.irq_spurious - s_rd0.irq_spurious);
    lb_ku("wakes", s_cs1.irq_wakes - s_cs0.irq_wakes);
    lb_ku("runs", s_ts1.ctrl_runs - s_ts0.ctrl_runs);
    lb_ku("cpu_us", s_ts1.ctrl_run_total_us - s_ts0.ctrl_run_total_us);
    lb_ku("lat_max", s_rd1.irq_lat_max_us);
    lb_ku("lat_avg", s_rd1.irq_lat_n ? s_rd1.irq_lat_sum_us / s_rd1.irq_lat_n : 0u);
    lb_ku("svc_max", s_rd1.svc_max_us);
    lb_ku("disp_max", s_rd1.disp_max_us);
    lb_ku("disp_ev", s_rd1.disp_max_ev);
    lb_ku("run_max", s_rd1.run_max_us);
    lb_ku("ctrl_max", s_ts1.ctrl_run_max_us);
    lb_ku("bh_max", s_cs1.bh_loops_max);
    lb_emit();

    /* per bit: top-half occurrences / bottom-half services / latched again while pending */
    lb_begin("IRQBITS");
    uint32_t shown = 0u;
    for (uint32_t b = 0u; b < 32u; b++) {
        uint32_t occ = s_rs1.irq_count[b] - s_rs0.irq_count[b];
        uint32_t svc = s_cs1.bh_serviced[b] - s_cs0.bh_serviced[b];
        uint32_t coal = s_rd1.irq_coalesced[b] - s_rd0.irq_coalesced[b];
        if (occ == 0u && svc == 0u) {
            continue;
        }
        lb_c(' ');
        lb_c('b');
        lb_u(b);
        lb_c('=');
        lb_u(occ);
        lb_c('/');
        lb_u(svc);
        lb_c('/');
        lb_u(coal);
        shown++;
    }
    if (shown == 0u) {
        lb_s(" none");
    }
    lb_kx("unh", s_rs1.irq_unhandled, 8);
    lb_emit();
}

static void log_summary(void)
{
    snapshot(&s_cs1, &s_rs1, &s_rd1, &s_ts1);
    lb_begin("HEAP");
    lb_ku("bytes", s_rd1.heap_bytes);
    lb_ku("hw", s_rd1.heap_hw);
    lb_ku("used", s_rd1.heap_used);
    lb_ku("largest", s_rs1.heap_largest);
    lb_ku("fail", s_rd1.alloc_fail);
    lb_ku("refused", s_rd1.alloc_refused);
    lb_ku("badfree", s_rd1.bad_free);
    lb_ku("rtos_free", openfw_rtos_heap_free());
    lb_emit();

    lb_begin("TIME");
    lb_ku("rfcal", s_rd1.rf_cal_us);
    lb_ku("start", s_rd1.start_us);
    lb_ku("stop", s_rd1.stop_us);
    lb_ku("ctrl_max_all", s_ts1.ctrl_run_max_all_us);
    lb_ku("starts", s_rd1.starts);
    lb_ku("stops", s_rd1.stops);
    lb_ku("stop_to", s_rd1.stop_timeouts);
    lb_emit();

    lb_begin("CTRL");
    lb_ku("rom_msgs", s_cs1.rom_msgs);
    lb_ku("rom_unk", s_cs1.rom_unknown_ids);
    lb_ku("rom_short", s_cs1.rom_short);
    lb_ku("late", s_cs1.late_upcalls);
    lb_ku("resets", s_cs1.resets);
    lb_ku("rfail", s_cs1.reset_failures);
    lb_ku("hung", s_ts1.hung);
    lb_ku("faults", s_ts1.faults);
    lb_ku("first", s_ts1.first_fault);
    lb_ku("inv", s_cs1.invariant);
    lb_ku("lockv", s_cs1.lock_violations + s_ts1.lock_violations);
    lb_ku("eif", s_rd1.eif_writes);
    lb_ku("evt_unsol", s_evt_unsolicited);
    lb_ku("rx_drop", s_rx_drop);
    lb_emit();

    lb_begin("STACK");
    lb_ku("ctrl_size", BT_CTRL_RTOS_CTRL_STACK_WORDS * 4u);
    lb_ku("ctrl_free", s_ts1.ctrl_stack_free_words * 4u);
    lb_ku("dlv_free", s_ts1.dlv_stack_free_words * 4u);
    lb_ku("probe_free", (uint32_t)uxTaskGetStackHighWaterMark(NULL) * 4u);
    lb_emit();
}

/* What the probe leaves behind for the legacy path: IRQ-26 priority (restored by stop()) and whether RW-BT may be in
 * an unknown state (legacy BT mode then stays refused until reboot). */
static void log_release(void)
{
    bt_rom_real_diag(&s_rd1);
    lb_begin("RELEASE");
    lb_kx("ipr", s_rd1.ipr_now, 2);
    lb_kx("orig", s_rd1.ipr_orig, 2);
    lb_ku("claimed", s_rd1.claimed);
    lb_ku("owner", s_rd1.owner);
    lb_ku("lockout", s_lockout);
    lb_ks("legacy", hci_probe_active_after_end() ? "refused" : "allowed");
    lb_emit();
}

/* ---- stage B ---- */
static uint8_t s_name[BT_CTRL_NAME_LEN] PROBE_BSS;   /* 0C13 parameter: probe name, zero padded */
static char s_name_str[24];

/* Bluedroid's Classic event mask on the wire (controller.c:477, HCI_LISBON_EVENT_MASK_EXT via ARRAY8_TO_STREAM;
 * BLE_INCLUDED is FALSE in this build). */
static const uint8_t k_evt_mask_bd[8] = { 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xBFu, 0x1Du };
static const uint8_t k_evt_mask_zero[8] = { 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u };
static const uint8_t k_cod[3] = { 0x04u, 0x04u, 0x20u };    /* legacy F:1550: Audio/Video, portable audio; Audio */

/* Events after a stage-B reply: none is expected (every V-B row is a CC with no terminal event), so whatever arrives
 * within SETTLE_MS is logged as EVT right after the CMD line. */
static void settle(void)
{
    wait_ms(SETTLE_MS);
    while (rx_pop(&s_pop)) {
        log_unsolicited(&s_pop);
    }
}

static bool cmd_b(uint16_t op, const uint8_t *prm, uint8_t plen)
{
    int r = do_cmd(op, prm, plen, false, NULL);
    settle();
    return r == R_OK;
}

static void verify_end(bool match, uint32_t lat)
{
    if (!match) {
        s_vfail++;
    }
    lb_ku("lat", lat);
    lb_ks("res", match ? "match" : "MISMATCH");
    lb_emit();
}

/* 1004 page 1 after 0C56: features[0] bit 0 = SSP host support, set by bt_ctrl only when the ROM answered s=00. */
static bool verify_ssp(void)
{
    static const uint8_t page1 = 1u;
    uint32_t lat = 0u;
    if (do_cmd(0x1004u, &page1, 1u, true, &lat) != R_OK) {
        return false;
    }
    uint8_t got = s_reply.len >= 10u ? s_reply.data[9] : 0xFFu;
    lb_begin("VERIFY");
    lb_ks("what", "ssp");
    lb_s(" op=1004 p=01 got=");
    lb_x(got, 2);
    lb_s(" want=01");
    verify_end(got == 0x01u, lat);
    return true;
}

/* hci_tl_env.evt_msk after 0C01 (plain RAM read): the ROM handler passes the parameter to hook 0x00803BBC[7], which
 * stores mask | mandatory. */
static bool evt_mask_step(const uint8_t mask[8])
{
    static const uint8_t mandatory[8] = BT_ROM_REAL_EVT_MASK_MANDATORY;
    uint8_t was[8], got[8], want[8];
    bt_rom_real_evt_mask(was);
    if (!cmd_b(0x0C01u, mask, 8u)) {
        return false;
    }
    bt_rom_real_evt_mask(got);
    for (uint32_t i = 0u; i < 8u; i++) {
        want[i] = (uint8_t)(mask[i] | mandatory[i]);
    }
    lb_begin("VERIFY");
    lb_ks("what", "evtmask");
    lb_s(" was=");
    lb_hex(was, 8u);
    lb_s(" got=");
    lb_hex(got, 8u);
    lb_s(" want=");
    lb_hex(want, 8u);
    verify_end(memcmp(got, want, 8u) == 0, 0u);
    return true;
}

/* 0C14 after 0C13: the reply carries the 248-byte name from bt_ctrl's cache, updated only on the ROM's s=00. */
static bool verify_name(void)
{
    uint32_t lat = 0u;
    if (do_cmd(0x0C14u, NULL, 0u, true, &lat) != R_OK) {
        return false;
    }
    bool match = s_reply.len >= 7u + BT_CTRL_NAME_LEN && memcmp(&s_reply.data[7], s_name, BT_CTRL_NAME_LEN) == 0;
    lb_begin("VERIFY");
    lb_ks("what", "name");
    lb_s(" op=0C14");
    lb_ks("name", s_name_str);
    verify_end(match, lat);
    return true;
}

/* Stage B measurement window: the stage A idle-window counters plus unsolicited events, ROM messages to TASK_HCI,
 * g_bt_heap, the highest bt_ctrl CPU share in one 100 ms guard sample (cpu_pk, permille) and the bt_ctrl stack
 * watermark. Events are drained every 100 ms (the stub ring has 3 slots). Ends early on a bt_ctrl fault, a hang or the
 * CPU-share guard (end=fault|hang|cpu). */
static bool measure_window(const char *w, uint32_t ms)
{
    bt_rom_real_diag_reset_max();
    bt_ctrl_rtos_reset_max();
    snapshot(&s_cs0, &s_rs0, &s_rd0, &s_ts0);
    uint32_t ev0 = s_evt_unsolicited;
    uint32_t t0 = now32();
    const char *end = NULL;
    for (uint32_t el = 0u; el < ms; el += 100u) {
        wait_ms(100u);
        while (rx_pop(&s_pop)) {
            log_unsolicited(&s_pop);
        }
        bt_ctrl_rtos_stats(&s_ts1);
        if (s_ts1.faults != s_ts0.faults || s_ts1.hung != 0u) {
            end = s_ts1.cpu_tripped ? "cpu" : (s_ts1.hung ? "hang" : "fault");
            break;
        }
    }
    bool ok = end == NULL;
    uint32_t win = now32() - t0;
    snapshot(&s_cs1, &s_rs1, &s_rd1, &s_ts1);

    lb_begin("WIN");
    lb_ks("w", w);
    lb_ku("us", win);
    lb_ku("irq", s_rd1.irq_total - s_rd0.irq_total);
    lb_ku("spur", s_rd1.irq_spurious - s_rd0.irq_spurious);
    lb_ku("wakes", s_cs1.irq_wakes - s_cs0.irq_wakes);
    lb_ku("runs", s_ts1.ctrl_runs - s_ts0.ctrl_runs);
    lb_ku("cpu_us", s_ts1.ctrl_run_total_us - s_ts0.ctrl_run_total_us);
    lb_ku("lat_max", s_rd1.irq_lat_max_us);
    lb_ku("lat_avg", s_rd1.irq_lat_n ? s_rd1.irq_lat_sum_us / s_rd1.irq_lat_n : 0u);
    lb_ku("svc_max", s_rd1.svc_max_us);
    lb_ku("disp_max", s_rd1.disp_max_us);
    lb_ku("run_max", s_rd1.run_max_us);
    lb_ku("ctrl_max", s_ts1.ctrl_run_max_us);
    lb_ku("bh_max", s_cs1.bh_loops_max);
    lb_ku("evts", s_evt_unsolicited - ev0);
    lb_ku("romd", s_cs1.rom_msgs - s_cs0.rom_msgs);
    lb_ku("unk", s_cs1.rom_unknown_ids - s_cs0.rom_unknown_ids);
    lb_ku("hw", s_rd1.heap_hw);
    lb_ku("used", s_rd1.heap_used);
    lb_ku("cpu_pk", s_ts1.cpu_peak_pm);
    lb_ku("ctrl_free", s_ts1.ctrl_stack_free_words * 4u);
    if (!ok) {
        lb_ks("end", end);
    }
    lb_emit();

    /* per bit, as IRQBITS: top-half occurrences / bottom-half services / latched again while pending */
    uint32_t shown = 0u;
    for (uint32_t b = 0u; b < 32u; b++) {
        uint32_t occ = s_rs1.irq_count[b] - s_rs0.irq_count[b];
        uint32_t svc = s_cs1.bh_serviced[b] - s_cs0.bh_serviced[b];
        uint32_t coal = s_rd1.irq_coalesced[b] - s_rd0.irq_coalesced[b];
        if (occ == 0u && svc == 0u) {
            continue;
        }
        if (shown % 6u == 0u) {              /* <= 6 bits per line keeps every line inside s_line */
            if (shown != 0u) {
                lb_emit();
            }
            lb_begin("WINBITS");
            lb_ks("w", w);
        }
        lb_c(' ');
        lb_c('b');
        lb_u(b);
        lb_c('=');
        lb_u(occ);
        lb_c('/');
        lb_u(svc);
        lb_c('/');
        lb_u(coal);
        shown++;
    }
    if (shown == 0u) {
        lb_begin("WINBITS");
        lb_ks("w", w);
        lb_s(" none");
    }
    lb_kx("unh", s_rs1.irq_unhandled, 8);
    lb_emit();
    return ok;
}

/* Stage B on the session stage A's last restart left up. Returns false after a failure (already torn down). */
static bool stage_b(void)
{
    static const uint8_t one = 0x01u, scan_on = 0x03u, scan_off = 0x00u;
    uint32_t t0 = now32();
    const char *why = "stageB";
    char *q = s_name_str;
    for (const char *p = PROBE_NAME_PREFIX; *p != '\0'; p++) {
        *q++ = *p;
    }
    static const char hx[] = "0123456789ABCDEF";
    const uint8_t tail[2] = { s_bd[1], s_bd[0] };          /* last two BD_ADDR bytes as displayed (wire is LSB first) */
    for (uint32_t i = 0u; i < 2u; i++) {
        *q++ = hx[tail[i] >> 4];
        *q++ = hx[tail[i] & 0xFu];
    }
    *q = '\0';
    memset(s_name, 0, sizeof(s_name));
    memcpy(s_name, s_name_str, (size_t)(q - s_name_str));

    s_bx = 1u;
    s_evt_logged = 0u;                       /* fresh EVT budget: stage B events matter most */
    bt_ctrl_rtos_cpu_guard_arm(1);           /* no Reset from here to the end of stage B */
    lb_begin("STAGE");
    lb_ks("s", "B");
    lb_ks("name", s_name_str);
    lb_ku("scan_ms", SCAN_WINDOW_MS);
    lb_ks("trig", s_trig);                   /* the ring keeps this line when BEGIN has wrapped out */
    lb_ku("cpu_pm", CPU_LIMIT_PM);
    lb_ku("cpu_n", CPU_SAMPLES);
    lb_emit();

    bool ok = cmd_b(0x0C56u, &one, 1u) && verify_ssp() &&
              evt_mask_step(k_evt_mask_bd) &&
              cmd_b(0x0C24u, k_cod, (uint8_t)sizeof(k_cod)) &&
              cmd_b(0x0C13u, s_name, (uint8_t)BT_CTRL_NAME_LEN) && verify_name();
    if (ok) {
        ok = cmd_b(0x0C1Au, &scan_on, 1u);
        if (ok) {
            ok = measure_window("scan", SCAN_WINDOW_MS);
            why = ok ? why : "scan_window";
            /* scan off even after a window fault, if bt_ctrl still answers (the teardown below stops it anyway); a
             * demoted bt_ctrl never answers: go straight to the hard path */
            bt_ctrl_rtos_stats(&s_ts1);
            if (!s_ts1.hung) {
                ok = cmd_b(0x0C1Au, &scan_off, 1u) && ok;
            }
        }
        ok = ok && measure_window("off", OFF_WINDOW_MS);
        ok = ok && evt_mask_step(k_evt_mask_zero) && evt_mask_step(k_evt_mask_bd);
    }
    s_bx = 0u;
    bt_ctrl_rtos_cpu_guard_arm(0);           /* a tripped guard stays latched (hung) */
    lb_begin("STAGE");
    lb_ks("s", "B");
    lb_ks("st", ok ? "DONE" : "FAIL");
    lb_ku("vfail", s_vfail);
    lb_ku("ms", (now32() - t0) / 1000u);
    lb_emit();
    if (!ok) {
        (void)fail(why);                     /* teardown (= ROM soft reset: scanning stops); no retry in stage B */
    }
    return ok;
}

/* Timer daemon (priority 9 > bt_ctrl 8): demote a bt_ctrl stuck inside one ROM call, or (stage B, armed) one that
 * keeps the CPU, so the app keeps running. */
static void guard_cb(TimerHandle_t t)
{
    (void)t;
    if (!bt_ctrl_rtos_guard(HANG_LIMIT_US)) {
        (void)bt_ctrl_rtos_cpu_guard(CPU_LIMIT_PM, CPU_SAMPLES);
    }
}

static void probe_task(void *arg)
{
    (void)arg;
    s_t_begin = now32();
    /* The previous boot ended with a fault record (a reset through system_reboot(): HardFault, configASSERT, stack
     * overflow, possibly inside a previous probe). Do not add a probe on top of it; the request is already consumed,
     * so the user decides whether to run again. A 32 s WDT reset leaves no record (PMU cold reset clears AON_BACKUP):
     * the one-shot request is what stops a repeat there. */
    if (g_prev_fault_code != 0u) {
        lb_begin("SKIP");
        lb_kx("prev_fault", g_prev_fault_code, 8);
        lb_ks("trig", s_trig);
        lb_emit();
        lb_begin("END");
        lb_ks("st", "SKIP");
        lb_emit();
        goto end;
    }
    /* Cleared at END. A reset in between that preserves AON (system_reboot) shows PREV_FAULT=0xFA0170B2 on the next
     * boot; a WDT PMU cold reset clears it (docs/SL6801_HW76_HARDWARE.md §5.2). */
    AON_BACKUP(0u) = PROBE_FAULT_MARK;

    const char *src = probe_bd_addr(s_bd);
    int err = bt_ctrl_rtos_init(&bt_rom_real, s_bd);
    lb_begin("BEGIN");
    lb_ku("v", 1u);
    lb_c(' ');
    lb_s("bd=");
    for (int i = 5; i >= 0; i--) {
        lb_x(s_bd[i], 2);
    }
    lb_ks("src", src);
    lb_ks("trig", s_trig);
    lb_ku("tblerr", (uint32_t)(err < 0 ? 999 : err));
    lb_ku("dl", CMD_DEADLINE_US);
    lb_ku("dl_reset", RESET_DEADLINE_US);
    lb_ku("hang", HANG_LIMIT_US);
    if (HCI_PROBE_WAIVE_LIVENESS) {
        lb_ku("waive_live", 1u);
    }
    lb_ks("stage", s_stage == HCIP_STAGE_B ? "B" : "A");
    lb_emit();
    log_not_probed();
    s_guard = xTimerCreateStatic("hcip_guard", pdMS_TO_TICKS(GUARD_PERIOD_MS), pdTRUE, NULL, guard_cb, &s_guard_buf);
    if (err < 0 || s_guard == NULL || xTimerStart(s_guard, 0) != pdPASS ||
        bt_ctrl_register_host_callback(&k_host) != 0) {
        lb_begin("ABORT");
        lb_ks("why", "init");
        lb_emit();
        goto done;
    }

    /* 1. Reset + liveness (+ one recovery) */
    bool ok = session_up(false, NULL, NULL);
    log_nvic();
    if (!ok) {
        goto abort;
    }

    /* 2. read-only SYN rows */
    static const struct { uint16_t op; uint8_t plen; uint8_t p0; } k_reads[] = {
        { 0x1005u, 0u, 0u }, { 0x1001u, 0u, 0u }, { 0x1009u, 0u, 0u }, { 0x1002u, 0u, 0u },
        { 0x1004u, 1u, 0u }, { 0x1004u, 1u, 1u }, { 0x0C14u, 0u, 0u },
    };
    for (uint32_t i = 0u; i < sizeof(k_reads) / sizeof(k_reads[0]); i++) {
        if (do_cmd(k_reads[i].op, &k_reads[i].p0, k_reads[i].plen, false, NULL) != R_OK) {
            if (!fail("cmd") || !session_up(false, NULL, NULL)) {
                goto abort;
            }
        }
    }

    /* 3. idle window */
    idle_window();

    /* 4. clean restart x5 */
    for (uint32_t i = 1u; i <= RESTARTS; i++) {
        int td = teardown("restart", true);
        if (td < 0) {
            goto abort;
        }
        bt_rom_real_diag(&s_rd1);
        uint32_t stop_us = s_rd1.stop_us;
        uint32_t rlat = 0u, blat = 0u, irqs = 0u;
        uint32_t f_before = s_failures;
        bool up = session_up(true, &rlat, &irqs);
        int br = up ? do_cmd(0x1009u, NULL, 0u, true, &blat) : R_TIMEOUT;
        bool bd_ok = br == R_OK && memcmp(&s_reply.data[7], s_bd, 6u) == 0;
        lb_begin("RESTART");
        lb_ku("i", i);
        lb_ks("stop", td == 0 ? "OK" : "ERR");
        lb_ku("stop_us", stop_us);
        lb_ks("reset", up ? "OK" : "FAIL");
        lb_ku("rlat", rlat);
        lb_ku("irq", irqs);
        lb_ks("bd", bd_ok ? "match" : (br == R_OK ? "MISMATCH" : k_res[br]));
        lb_ku("blat", blat);
        lb_ku("retries", s_failures - f_before);
        lb_emit();
        if (!up) {
            goto abort;
        }
        if (td != 0 || !bd_ok) {
            if (!fail("restart")) {
                goto abort;
            }
        }
    }

    /* 5. stage B (verified writes) on the session the last restart left up */
    if (s_stage == HCIP_STAGE_B && !stage_b()) {
        goto abort;
    }

    (void)teardown("end", false);
    log_summary();
    log_release();
    lb_begin("END");
    lb_ks("st", "DONE");
    lb_ku("fails", s_failures);
    lb_ku("ms", (now32() - s_t_begin) / 1000u);
    if (s_stage == HCIP_STAGE_B) {
        lb_ku("vfail", s_vfail);
    }
    lb_emit();
    goto done;

abort:
    {
        bt_ctrl_debug_t dbg;
        bt_ctrl_get_debug(&dbg);
        bt_ctrl_rtos_stats(&s_ts1);
        if (s_ts1.hung) {
            s_lockout = 1u;
            uint32_t cntl = bt_rom_real_silence();   /* no stop() will complete: RWBTEN off + soft reset, no ROM */
            lb_begin("HANG");
            lb_ks("why", s_ts1.cpu_tripped ? "cpu" : "run");
            lb_ku("limit", HANG_LIMIT_US);
            if (s_ts1.cpu_tripped) {
                lb_ku("cpu_pm", CPU_LIMIT_PM);
                lb_ku("cpu_n", CPU_SAMPLES);
                lb_ku("cpu_pk", s_ts1.cpu_peak_pm);
            }
            lb_ks("action", "bt_stop+irq26_masked+bt_ctrl_demoted+rwbt_silenced");
            lb_kx("cntl", cntl, 8);
            lb_emit();
        } else if (!s_hardstopped && dbg.state != 0u && !dbg.parked) {
            (void)teardown("abort", false);
        }
    }
    lb_begin("ABORT");
    lb_ku("fails", s_failures);
    lb_emit();
    log_summary();
    log_release();
    lb_begin("END");
    lb_ks("st", "ABORT");
    lb_ku("fails", s_failures);
    lb_ku("ms", (now32() - s_t_begin) / 1000u);
    lb_emit();
done:
    if (s_guard != NULL) {
        bt_ctrl_rtos_stats(&s_ts1);
        if (!s_ts1.hung) {
            (void)xTimerStop(s_guard, 0);    /* bt_ctrl is off; a demoted (hung) one keeps its guard latched */
        }
    }
    if (AON_BACKUP(0u) == PROBE_FAULT_MARK) {
        AON_BACKUP(0u) = 0u;                 /* finished (DONE or ABORT): not a fault for the next boot */
    }
end:
    s_log_ready = 1u;                        /* hci_probe_poll() saves the log copy, then clears s_active */
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

/* App task, once, after the SD mount: consume the one-shot request (same task as every other filesystem user). */
void hci_probe_start(exfat_vol_t *vol, int sd_mounted)
{
    if (s_decided) {
        return;                              /* once per boot */
    }
    s_decided = 1u;
    if (HCI_PROBE_AUTORUN || HCI_PROBE_WAIVE_LIVENESS) {
        rom_rst_release(0x58u);              /* eFuse reset / clock ids (bt_rf.c EFUSE_RST_ID / EFUSE_CLK_ID) */
        rom_clk_enable(0x1Bu);
        if (hci_probe_qemu_only_refused(1u, EFUSE_WORD(3))) {
            lb_begin("OFF");                 /* QEMU-only image on silicon: the request file is left untouched */
            lb_ks("trig", "qemu_only_on_silicon");
            lb_ks("file", HCI_PROBE_TRIGGER_FILE);
            lb_emit();
            return;
        }
    }
    uint8_t stage = HCI_PROBE_AUTORUN >= 2 ? HCIP_STAGE_B : HCIP_STAGE_A;
    hcip_trig_t t = HCI_PROBE_AUTORUN ? HCIP_TRIG_TAKEN : hci_probe_trigger_take(vol, sd_mounted, &stage);
    s_trig = HCI_PROBE_AUTORUN ? "autorun" : hci_probe_trigger_name(t);
    s_stage = stage;
    if (t != HCIP_TRIG_TAKEN) {
        lb_begin("OFF");
        lb_ks("trig", s_trig);
        lb_ks("file", HCI_PROBE_TRIGGER_FILE);
        lb_emit();
        return;
    }
    s_active = 1u;
    s_task = xTaskCreateStatic(probe_task, "hci_probe", PROBE_STACK_WORDS, NULL, PROBE_PRIO, s_stack, &s_tcb);
    if (s_task == NULL) {
        s_active = 0u;
    }
}

/* App task, every main-loop pass: after the probe task's END, write its log copy to SD:/HCIPROBE.LOG (the 4 KiB
 * g_openfw_log ring cannot hold stage A + B), then let recording / USB MSC / legacy BT go (s_active = 0). */
void hci_probe_poll(exfat_vol_t *vol, int sd_mounted)
{
    if (!s_log_ready) {
        return;
    }
    s_log_ready = 0u;
    uint32_t n = s_copy_len;
    int r = hci_probe_log_save(vol, sd_mounted, s_copy, n, &s_writer);
    lb_begin("LOGFILE");
    lb_ks("file", HCI_PROBE_LOG_FILE);
    lb_ks("st", r == 0 ? "OK" : "FAIL");
    lb_ku("bytes", n);
    lb_ku("lost", s_copy_lost);
    lb_emit();
    s_active = 0u;                           /* BT stays off for the rest of this boot */
}

static int hci_probe_active_after_end(void)
{
    return s_lockout != 0u || bt_rom_real_busy();
}

/* Legacy BT mode must not start: the probe runs, or it left RW-BT claimed (hang, hard stop, failed start/stop). */
int hci_probe_active(void)
{
    return s_active != 0u || hci_probe_active_after_end();
}

/* Recording and USB MSC wait for END and the log file: the probe's resets take the CPU in bursts of ~0.5 s. */
int hci_probe_running(void)
{
    return s_active != 0u;
}
