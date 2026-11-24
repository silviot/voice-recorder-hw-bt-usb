/* Host tests for the HCI probe's safety logic (plan M5 Exp 1 stages A and B):
 *   - bt_port/hci_probe_trigger.c: the probe runs only on a one-shot SD request file that is consumed (and the
 *     deletion verified) before the controller is touched, so a hang/WDT reset cannot turn into a reset loop; the
 *     file's content picks the stage (empty / "A" / "B"), anything else is consumed and refused; the probe's log copy
 *     replaces SD:/HCIPROBE.LOG;
 *   - bt_rom_real_msg_fits() (bt_port/bt_rom_real.h): msg_alloc admission must account for fragmentation, because the
 *     ROM's ke_msg_alloc never checks NULL;
 *   - bt_cpu_guard_step() (bt_port/bt_cpu_guard.h): the stage B CPU-share guard trips on sustained bt_ctrl load only
 *     (not on one Reset-length burst, a torn read or a late sample), so a re-latching IRQ 26 cannot starve the app/WDT;
 *   - hci_probe_qemu_only_refused(): a QEMU-only image (AUTORUN / WAIVE) refuses to run on a part with eFuses.
 * Usage: test_hci_probe <blank exFAT image> (made by mkfs.exfat in make test-bt-port).
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openfw_fs.h"
#include "hci_probe_trigger.h"
#include "bt_rom_real.h"
#include "bt_cpu_guard.h"

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- in-memory disk ---- */
static uint8_t *g_pristine, *g_disk;
static uint32_t g_sectors;
static int g_drop_writes;   /* 1: writes report success but change nothing (a card that does not persist) */

static int mem_read(void *ctx, uint32_t lba, uint32_t count, void *buf)
{
    (void)ctx;
    if (lba + count > g_sectors) {
        return -1;
    }
    memcpy(buf, &g_disk[(size_t)lba * 512u], (size_t)count * 512u);
    return 0;
}

static int mem_write(void *ctx, uint32_t lba, uint32_t count, const void *buf)
{
    (void)ctx;
    if (lba + count > g_sectors) {
        return -1;
    }
    if (!g_drop_writes) {
        memcpy(&g_disk[(size_t)lba * 512u], buf, (size_t)count * 512u);
    }
    return 0;
}

static blkdev_t g_rw, g_ro;
static exfat_vol_t g_vol;

static void fresh(const blkdev_t *dev)
{
    memcpy(g_disk, g_pristine, (size_t)g_sectors * 512u);
    g_drop_writes = 0;
    if (exfat_mount(&g_vol, dev) != 0) {
        printf("FAIL: exfat_mount\n");
        exit(1);
    }
}

static void put_data(const char *name, const void *data, uint32_t len)
{
    exfat_writer_t w;
    CHECK(exfat_create_file(&g_vol, "/", name, &w) == 0, "create %s", name);
    CHECK(len == 0u || exfat_write(&w, data, len) == 0, "write %s", name);
    CHECK(exfat_close_file(&w) == 0, "close %s", name);
}

static void put_file(const char *name)              /* a stage-A request ("A") or any other file */
{
    put_data(name, "A", 1u);
}

/* Content of a root file, NUL-terminated; -1 if absent. */
static int read_back(const char *name, char *buf, uint32_t cap)
{
    exfat_entry_t e;
    char path[80];
    uint32_t got = 0u;
    snprintf(path, sizeof(path), "/%s", name);
    if (exfat_find_path(&g_vol, path, &e) != 0 || e.size >= cap ||
        exfat_read_file(&g_vol, &e, 0u, buf, (uint32_t)e.size, &got) != 0 || got != e.size) {
        return -1;
    }
    buf[got] = '\0';
    return (int)got;
}

static int exists(const char *path)
{
    exfat_entry_t e;
    return exfat_find_path(&g_vol, path, &e) == 0;
}

static hcip_trig_t take(exfat_vol_t *vol, int mounted)
{
    uint8_t st = 0xAAu;
    hcip_trig_t t = hci_probe_trigger_take(vol, mounted, &st);
    CHECK((t == HCIP_TRIG_TAKEN) == (st != HCIP_STAGE_NONE), "stage set exactly when taken (t=%d st=%u)", (int)t, st);
    return t;
}

static void test_trigger(void)
{
    /* no volume */
    fresh(&g_rw);
    CHECK(take(&g_vol, 0) == HCIP_TRIG_NO_SD, "unmounted");
    CHECK(take(NULL, 1) == HCIP_TRIG_NO_SD, "NULL volume");
    CHECK(hci_probe_trigger_take(NULL, 1, NULL) == HCIP_TRIG_NO_SD, "NULL stage pointer");

    /* blank card: no request, nothing written */
    fresh(&g_rw);
    CHECK(take(&g_vol, 1) == HCIP_TRIG_ABSENT, "blank card");
    CHECK(memcmp(g_disk, g_pristine, (size_t)g_sectors * 512u) == 0, "absent request must not write the card");

    /* request present: taken once, then gone (a reset during the probe cannot rerun it), other files untouched */
    fresh(&g_rw);
    put_file("KEEP.TXT");
    put_file(HCI_PROBE_TRIGGER_FILE);
    CHECK(take(&g_vol, 1) == HCIP_TRIG_TAKEN, "request taken");
    CHECK(!exists("/" HCI_PROBE_TRIGGER_FILE), "request deleted");
    CHECK(exists("/KEEP.TXT"), "other file kept");
    CHECK(take(&g_vol, 1) == HCIP_TRIG_ABSENT, "second boot: no request");
    exfat_vol_t again;                                       /* "next boot": remount from the card */
    CHECK(exfat_mount(&again, &g_rw) == 0, "remount");
    CHECK(take(&again, 1) == HCIP_TRIG_ABSENT, "remounted: no request");

    /* names are case-insensitive (a file created as hciprobe.run on a PC) */
    fresh(&g_rw);
    put_file("hciprobe.run");
    CHECK(take(&g_vol, 1) == HCIP_TRIG_TAKEN, "lower-case request taken");
    CHECK(!exists("/hciprobe.run"), "lower-case request deleted");

    /* a directory of that name is not a request and is left alone */
    fresh(&g_rw);
    CHECK(exfat_mkdir(&g_vol, HCI_PROBE_TRIGGER_FILE) == 0, "mkdir");
    CHECK(take(&g_vol, 1) == HCIP_TRIG_NOT_FILE, "directory");
    CHECK(exists("/" HCI_PROBE_TRIGGER_FILE), "directory kept");

    /* request in a subdirectory only: not a request */
    fresh(&g_rw);
    CHECK(exfat_mkdir(&g_vol, "SUB") == 0, "mkdir SUB");
    exfat_writer_t w;
    CHECK(exfat_create_file(&g_vol, "/SUB", HCI_PROBE_TRIGGER_FILE, &w) == 0 && exfat_close_file(&w) == 0, "sub file");
    CHECK(take(&g_vol, 1) == HCIP_TRIG_ABSENT, "subdirectory request ignored");

    /* request that cannot be consumed: never run (it would run again on every boot) */
    fresh(&g_rw);
    put_file(HCI_PROBE_TRIGGER_FILE);
    exfat_vol_t ro;
    CHECK(exfat_mount(&ro, &g_ro) == 0, "read-only mount");
    CHECK(take(&ro, 1) == HCIP_TRIG_STUCK, "read-only card");
    CHECK(exists("/" HCI_PROBE_TRIGGER_FILE), "read-only: still there");

    fresh(&g_rw);
    put_file(HCI_PROBE_TRIGGER_FILE);
    g_drop_writes = 1;                                       /* delete "succeeds" but does not persist */
    CHECK(take(&g_vol, 1) == HCIP_TRIG_STUCK, "delete that does not stick");
    g_drop_writes = 0;

    CHECK(strcmp(hci_probe_trigger_name(HCIP_TRIG_TAKEN), "taken") == 0 &&
          strcmp(hci_probe_trigger_name(HCIP_TRIG_ABSENT), "absent") == 0 &&
          strcmp(hci_probe_trigger_name(HCIP_TRIG_STUCK), "stuck") == 0 &&
          strcmp(hci_probe_trigger_name(HCIP_TRIG_BAD), "bad_content") == 0, "names");
}

/* The request file's content selects the stage; it is consumed whatever it says. */
static void test_stage(void)
{
    static const struct { const char *data; uint32_t len; uint8_t stage; } k[] = {
        { "", 0u, HCIP_STAGE_A },                     /* touch HCIPROBE.RUN (the stage A instructions) */
        { "A", 1u, HCIP_STAGE_A }, { "a\n", 2u, HCIP_STAGE_A }, { "\r\n", 2u, HCIP_STAGE_A },
        { "B", 1u, HCIP_STAGE_B }, { "b\n", 2u, HCIP_STAGE_B }, { "  B \r\n", 6u, HCIP_STAGE_B },
        { "\xEF\xBB\xBF" "B\r\n", 6u, HCIP_STAGE_B },   /* Windows Notepad: BOM + CRLF */
        { "C", 1u, HCIP_STAGE_NONE }, { "AB", 2u, HCIP_STAGE_NONE }, { "A B", 3u, HCIP_STAGE_NONE },
        { "x", 1u, HCIP_STAGE_NONE }, { "stage B", 7u, HCIP_STAGE_NONE }, { "B\0", 2u, HCIP_STAGE_NONE },
    };
    for (size_t i = 0u; i < sizeof(k) / sizeof(k[0]); i++) {
        CHECK(hci_probe_trigger_parse((const uint8_t *)k[i].data, k[i].len) == k[i].stage, "parse #%zu", i);
        fresh(&g_rw);
        put_data(HCI_PROBE_TRIGGER_FILE, k[i].data, k[i].len);
        uint8_t st = 0xAAu;
        hcip_trig_t t = hci_probe_trigger_take(&g_vol, 1, &st);
        if (k[i].stage == HCIP_STAGE_NONE) {
            CHECK(t == HCIP_TRIG_BAD && st == HCIP_STAGE_NONE, "bad content #%zu refused (t=%d)", i, (int)t);
        } else {
            CHECK(t == HCIP_TRIG_TAKEN && st == k[i].stage, "content #%zu -> stage %u (t=%d st=%u)", i,
                  k[i].stage, (int)t, st);
        }
        CHECK(!exists("/" HCI_PROBE_TRIGGER_FILE), "content #%zu consumed", i);
        CHECK(take(&g_vol, 1) == HCIP_TRIG_ABSENT, "content #%zu: next boot has no request", i);
    }
    CHECK(hci_probe_trigger_parse(NULL, 0u) == HCIP_STAGE_A, "NULL, 0");

    /* oversized request: not a stage word, refused but consumed */
    char big[HCI_PROBE_TRIGGER_MAX + 8u];
    memset(big, 'B', sizeof(big));
    fresh(&g_rw);
    put_data(HCI_PROBE_TRIGGER_FILE, big, (uint32_t)sizeof(big));
    CHECK(take(&g_vol, 1) == HCIP_TRIG_BAD, "oversized request refused");
    CHECK(!exists("/" HCI_PROBE_TRIGGER_FILE), "oversized request consumed");

    /* a stage-B request on a read-only card still never runs */
    fresh(&g_rw);
    put_data(HCI_PROBE_TRIGGER_FILE, "B", 1u);
    exfat_vol_t ro;
    CHECK(exfat_mount(&ro, &g_ro) == 0, "read-only mount");
    CHECK(take(&ro, 1) == HCIP_TRIG_STUCK, "read-only stage B request");
}

/* SD:/HCIPROBE.LOG: written after the probe's END, replaced by the next run, other files untouched. */
static void test_log_save(void)
{
    exfat_writer_t w;
    char buf[8192];
    static char big[6144];
    for (size_t i = 0u; i < sizeof(big); i++) {
        big[i] = (i % 97u == 96u) ? '\n' : (char)('A' + (int)(i % 26u));
    }
    fresh(&g_rw);
    put_file("KEEP.TXT");
    CHECK(hci_probe_log_save(&g_vol, 1, "HCIP BEGIN v=1\n", 15u, &w) == 0, "save");
    CHECK(read_back(HCI_PROBE_LOG_FILE, buf, sizeof(buf)) == 15 && strcmp(buf, "HCIP BEGIN v=1\n") == 0, "content");
    CHECK(hci_probe_log_save(&g_vol, 1, big, (uint32_t)sizeof(big), &w) == 0, "replace with a 6 KiB log");
    CHECK(read_back(HCI_PROBE_LOG_FILE, buf, sizeof(buf)) == (int)sizeof(big) && memcmp(buf, big, sizeof(big)) == 0,
          "replaced content (spans sectors/clusters)");
    CHECK(hci_probe_log_save(&g_vol, 1, "", 0u, &w) == 0 && read_back(HCI_PROBE_LOG_FILE, buf, sizeof(buf)) == 0,
          "empty log");
    CHECK(exists("/KEEP.TXT"), "other file kept");
    exfat_vol_t again;                                       /* survives a remount (next boot / USB host) */
    CHECK(exfat_mount(&again, &g_rw) == 0 && exfat_find_path(&again, "/" HCI_PROBE_LOG_FILE, &(exfat_entry_t){0}) == 0,
          "remount sees the log");

    CHECK(hci_probe_log_save(&g_vol, 0, "x", 1u, &w) == -1, "unmounted");
    CHECK(hci_probe_log_save(NULL, 1, "x", 1u, &w) == -1, "NULL volume");
    CHECK(hci_probe_log_save(&g_vol, 1, "x", 1u, NULL) == -1, "NULL writer");

    /* a directory of that name is never replaced */
    fresh(&g_rw);
    CHECK(exfat_mkdir(&g_vol, HCI_PROBE_LOG_FILE) == 0, "mkdir log name");
    CHECK(hci_probe_log_save(&g_vol, 1, "x", 1u, &w) == -1, "directory refused");
    exfat_entry_t e;
    CHECK(exfat_find_path(&g_vol, "/" HCI_PROBE_LOG_FILE, &e) == 0 && e.is_dir, "directory kept");

    /* read-only card */
    fresh(&g_rw);
    exfat_vol_t ro;
    CHECK(exfat_mount(&ro, &g_ro) == 0, "read-only mount");
    CHECK(hci_probe_log_save(&ro, 1, "x", 1u, &w) == -1, "read-only card");
}

static void test_msg_fits(void)
{
    const uint32_t H = BT_ROM_REAL_HEAP_BYTES, R = BT_ROM_REAL_HEAP_RESERVE, B = BT_ROM_REAL_BLK_HDR;
    /* 0C13-sized message: 248 B param -> 16 + 248 = 264 B payload (8-aligned), 272 B block */
    const uint16_t len = 248u;
    const uint32_t payload = 264u, need = payload + B;

    CHECK(bt_rom_real_msg_fits(0u, H - B, len), "empty heap");
    /* total budget boundary (unfragmented) */
    CHECK(bt_rom_real_msg_fits(H - R - need, H, len), "total at the limit");
    CHECK(!bt_rom_real_msg_fits(H - R - need + 8u, H, len), "total over the limit");
    /* fragmentation: 4 KiB in use, total well under budget, but the largest free run is 1000 B. The old total-only
     * check admitted this; the ROM's ke_malloc would then have to carve 272 B and leave < reserve contiguous. */
    CHECK(4096u + need + R <= H, "scenario is under the total budget");
    CHECK(!bt_rom_real_msg_fits(4096u, 1000u, len), "fragmented heap refused");
    /* largest-run boundary */
    CHECK(bt_rom_real_msg_fits(4096u, payload + R, len), "largest run at the limit");
    CHECK(!bt_rom_real_msg_fits(4096u, payload + R - 1u, len), "largest run one byte short");
    /* largest run smaller than the message itself (would be a NULL from ke_malloc) */
    CHECK(!bt_rom_real_msg_fits(0u, payload - 8u, len), "message does not fit any run");
    /* zero-length message still needs its 16 B ke_msg header */
    CHECK(bt_rom_real_msg_fits(0u, 16u + R, 0u) && !bt_rom_real_msg_fits(0u, 15u + R, 0u), "header-only message");
}

/* Feed the guard 100 ms samples with `pm` permille of bt_ctrl load each; returns the sample index (1-based) that
 * tripped, 0 if none did. */
static uint32_t g_now, g_busy;
static uint32_t feed(bt_cpu_guard_t *g, uint32_t n, uint32_t pm)
{
    uint32_t tripped = 0u;
    for (uint32_t i = 1u; i <= n; i++) {
        g_now += 100000u;
        g_busy += pm * 100u;
        if (bt_cpu_guard_step(g, g_now, g_busy, 700u, 10u) && tripped == 0u) {
            tripped = i;
        }
    }
    return tripped;
}

static void test_cpu_guard(void)
{
    bt_cpu_guard_t g;
    bt_cpu_guard_reset(&g);
    g_now = 0xFFF00000u;                     /* both counters wrap during the test */
    g_busy = 0xFFFF0000u;
    CHECK(bt_cpu_guard_step(&g, g_now, g_busy, 700u, 10u) == 0 && g.valid, "first call is the baseline");
    /* stage A idle (5.3 %) and a busy scan window well under the limit never trip */
    CHECK(feed(&g, 300u, 53u) == 0u && g.peak_pm == 53u, "idle load (peak %u)", g.peak_pm);
    CHECK(feed(&g, 300u, 650u) == 0u && g.peak_pm == 650u, "65 %% load");
    /* one Reset-length burst (RF calibration ~0.5 s at 100 %) does not trip; 9 samples over the limit do not either */
    CHECK(feed(&g, 5u, 1000u) == 0u && feed(&g, 1u, 0u) == 0u, "0.5 s burst");
    CHECK(feed(&g, 9u, 800u) == 0u && feed(&g, 1u, 100u) == 0u, "9 samples over, then under");
    /* sustained load: trips on the 10th consecutive sample over 70 %, and stays tripped */
    CHECK(feed(&g, 12u, 750u) == 10u, "sustained 75 %% trips at sample 10");
    CHECK(bt_cpu_guard_step(&g, g_now + 100000u, g_busy + 100000u, 700u, 10u) == 1, "still over: still tripped");
    /* exactly at the limit is not over */
    bt_cpu_guard_reset(&g);
    (void)bt_cpu_guard_step(&g, g_now, g_busy, 700u, 10u);
    CHECK(feed(&g, 20u, 700u) == 0u, "70 %% exactly does not trip");
    /* a busy counter that goes backwards (torn read) is no load, not 100 % */
    bt_cpu_guard_reset(&g);
    (void)bt_cpu_guard_step(&g, g_now, g_busy, 700u, 10u);
    g_now += 100000u;
    g_busy -= 50u;
    CHECK(bt_cpu_guard_step(&g, g_now, g_busy, 700u, 1u) == 0 && g.peak_pm == 0u, "backwards busy = 0 %%");
    /* more busy than elapsed (run-length rounding) is capped at 100 % */
    g_now += 100000u;
    g_busy += 100050u;
    CHECK(bt_cpu_guard_step(&g, g_now, g_busy, 700u, 1u) == 1 && g.peak_pm == 1000u, "capped at 1000 permille");
    /* a sample < 1 ms after the previous one is merged into the next one (no division by a tiny interval) */
    bt_cpu_guard_reset(&g);
    (void)bt_cpu_guard_step(&g, g_now, g_busy, 700u, 10u);
    CHECK(bt_cpu_guard_step(&g, g_now + 500u, g_busy + 500u, 700u, 1u) == 0 && g.over == 0u, "short sample skipped");
    CHECK(bt_cpu_guard_step(&g, g_now + 100000u, g_busy + 10000u, 700u, 1u) == 0 && g.peak_pm == 100u,
          "merged sample measured over the whole interval (peak %u)", g.peak_pm);
    /* a late sample (timer daemon delayed 10 s) does not overflow the arithmetic */
    bt_cpu_guard_reset(&g);
    (void)bt_cpu_guard_step(&g, 0u, 0u, 700u, 10u);
    CHECK(bt_cpu_guard_step(&g, 10000000u, 9000000u, 700u, 1u) == 1 && g.peak_pm == 900u, "10 s sample at 90 %%");
    /* samples = 0 never trips */
    bt_cpu_guard_reset(&g);
    (void)bt_cpu_guard_step(&g, 0u, 0u, 700u, 0u);
    CHECK(bt_cpu_guard_step(&g, 100000u, 100000u, 700u, 0u) == 0, "samples=0 disables");
}

static void test_qemu_only(void)
{
    CHECK(hci_probe_qemu_only_refused(1u, 0xC0000440u) == 1, "HW_76 eFuse w3: refused");
    CHECK(hci_probe_qemu_only_refused(1u, 0x80000000u) == 1, "valid bit alone: refused");
    CHECK(hci_probe_qemu_only_refused(1u, 0u) == 0, "QEMU (no eFuse model, reads 0): allowed");
    CHECK(hci_probe_qemu_only_refused(1u, 0x7FFFFFFFu) == 0, "eFuse not programmed: allowed (fails open)");
    CHECK(hci_probe_qemu_only_refused(0u, 0xC0000440u) == 0, "silicon image: never refused here");
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        printf("usage: %s <blank exFAT image>\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (f == NULL) {
        printf("cannot open %s\n", argv[1]);
        return 2;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || (n % 512) != 0) {
        fclose(f);
        printf("bad image size\n");
        return 2;
    }
    g_sectors = (uint32_t)(n / 512);
    g_pristine = malloc((size_t)n);
    g_disk = malloc((size_t)n);
    if (g_pristine == NULL || g_disk == NULL || fread(g_pristine, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        printf("cannot read image\n");
        return 2;
    }
    fclose(f);
    g_rw = (blkdev_t){ mem_read, mem_write, g_sectors, NULL };
    g_ro = (blkdev_t){ mem_read, NULL, g_sectors, NULL };

    test_trigger();
    test_stage();
    test_log_save();
    test_msg_fits();
    test_cpu_guard();
    test_qemu_only();
    free(g_pristine);
    free(g_disk);
    if (g_fail) {
        printf("test_hci_probe: %d FAILED\n", g_fail);
        return 1;
    }
    printf("test_hci_probe: all passed (one-shot SD trigger, stage selection, log file, msg_alloc admission, CPU-share guard, "
           "QEMU-only refusal)\n");
    return 0;
}
