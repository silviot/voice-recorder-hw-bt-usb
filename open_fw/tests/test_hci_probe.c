/* Host tests for the HCI probe's safety logic (plan M5 Exp 1 stage A):
 *   - bt_port/hci_probe_trigger.c: the probe runs only on a one-shot SD request file that is consumed (and the
 *     deletion verified) before the controller is touched, so a hang/WDT reset cannot turn into a reset loop;
 *   - bt_rom_real_msg_fits() (bt_port/bt_rom_real.h): msg_alloc admission must account for fragmentation, because the
 *     ROM's ke_msg_alloc never checks NULL.
 * Usage: test_hci_probe <blank exFAT image> (made by mkfs.exfat in make test-bt-port).
 * SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openfw_fs.h"
#include "hci_probe_trigger.h"
#include "bt_rom_real.h"

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

static void put_file(const char *name)
{
    exfat_writer_t w;
    CHECK(exfat_create_file(&g_vol, "/", name, &w) == 0, "create %s", name);
    CHECK(exfat_write(&w, "x", 1u) == 0, "write %s", name);
    CHECK(exfat_close_file(&w) == 0, "close %s", name);
}

static int exists(const char *path)
{
    exfat_entry_t e;
    return exfat_find_path(&g_vol, path, &e) == 0;
}

static void test_trigger(void)
{
    /* no volume */
    fresh(&g_rw);
    CHECK(hci_probe_trigger_take(&g_vol, 0) == HCIP_TRIG_NO_SD, "unmounted");
    CHECK(hci_probe_trigger_take(NULL, 1) == HCIP_TRIG_NO_SD, "NULL volume");

    /* blank card: no request, nothing written */
    fresh(&g_rw);
    CHECK(hci_probe_trigger_take(&g_vol, 1) == HCIP_TRIG_ABSENT, "blank card");
    CHECK(memcmp(g_disk, g_pristine, (size_t)g_sectors * 512u) == 0, "absent request must not write the card");

    /* request present: taken once, then gone (a reset during the probe cannot rerun it), other files untouched */
    fresh(&g_rw);
    put_file("KEEP.TXT");
    put_file(HCI_PROBE_TRIGGER_FILE);
    CHECK(hci_probe_trigger_take(&g_vol, 1) == HCIP_TRIG_TAKEN, "request taken");
    CHECK(!exists("/" HCI_PROBE_TRIGGER_FILE), "request deleted");
    CHECK(exists("/KEEP.TXT"), "other file kept");
    CHECK(hci_probe_trigger_take(&g_vol, 1) == HCIP_TRIG_ABSENT, "second boot: no request");
    exfat_vol_t again;                                       /* "next boot": remount from the card */
    CHECK(exfat_mount(&again, &g_rw) == 0, "remount");
    CHECK(hci_probe_trigger_take(&again, 1) == HCIP_TRIG_ABSENT, "remounted: no request");

    /* names are case-insensitive (a file created as hciprobe.run on a PC) */
    fresh(&g_rw);
    put_file("hciprobe.run");
    CHECK(hci_probe_trigger_take(&g_vol, 1) == HCIP_TRIG_TAKEN, "lower-case request taken");
    CHECK(!exists("/hciprobe.run"), "lower-case request deleted");

    /* a directory of that name is not a request and is left alone */
    fresh(&g_rw);
    CHECK(exfat_mkdir(&g_vol, HCI_PROBE_TRIGGER_FILE) == 0, "mkdir");
    CHECK(hci_probe_trigger_take(&g_vol, 1) == HCIP_TRIG_NOT_FILE, "directory");
    CHECK(exists("/" HCI_PROBE_TRIGGER_FILE), "directory kept");

    /* request in a subdirectory only: not a request */
    fresh(&g_rw);
    CHECK(exfat_mkdir(&g_vol, "SUB") == 0, "mkdir SUB");
    exfat_writer_t w;
    CHECK(exfat_create_file(&g_vol, "/SUB", HCI_PROBE_TRIGGER_FILE, &w) == 0 && exfat_close_file(&w) == 0, "sub file");
    CHECK(hci_probe_trigger_take(&g_vol, 1) == HCIP_TRIG_ABSENT, "subdirectory request ignored");

    /* request that cannot be consumed: never run (it would run again on every boot) */
    fresh(&g_rw);
    put_file(HCI_PROBE_TRIGGER_FILE);
    exfat_vol_t ro;
    CHECK(exfat_mount(&ro, &g_ro) == 0, "read-only mount");
    CHECK(hci_probe_trigger_take(&ro, 1) == HCIP_TRIG_STUCK, "read-only card");
    CHECK(exists("/" HCI_PROBE_TRIGGER_FILE), "read-only: still there");

    fresh(&g_rw);
    put_file(HCI_PROBE_TRIGGER_FILE);
    g_drop_writes = 1;                                       /* delete "succeeds" but does not persist */
    CHECK(hci_probe_trigger_take(&g_vol, 1) == HCIP_TRIG_STUCK, "delete that does not stick");
    g_drop_writes = 0;

    CHECK(strcmp(hci_probe_trigger_name(HCIP_TRIG_TAKEN), "taken") == 0 &&
          strcmp(hci_probe_trigger_name(HCIP_TRIG_ABSENT), "absent") == 0 &&
          strcmp(hci_probe_trigger_name(HCIP_TRIG_STUCK), "stuck") == 0, "names");
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
    test_msg_fits();
    free(g_pristine);
    free(g_disk);
    if (g_fail) {
        printf("test_hci_probe: %d FAILED\n", g_fail);
        return 1;
    }
    printf("test_hci_probe: all passed (one-shot SD trigger, msg_alloc admission)\n");
    return 0;
}
