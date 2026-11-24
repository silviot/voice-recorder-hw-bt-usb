/* hci_probe_trigger: one-shot SD request file for the HCI probe (see hci_probe_trigger.h).
 * SPDX-License-Identifier: MIT */
#include <stddef.h>
#include "hci_probe_trigger.h"

static int is_space(uint8_t c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

uint8_t hci_probe_trigger_parse(const uint8_t *buf, uint32_t len)
{
    uint32_t a = 0u, b = len;
    if (buf == NULL) {
        return len == 0u ? HCIP_STAGE_A : HCIP_STAGE_NONE;
    }
    if (len >= 3u && buf[0] == 0xEFu && buf[1] == 0xBBu && buf[2] == 0xBFu) {
        a = 3u;                                          /* UTF-8 BOM (Windows editors) */
    }
    while (a < b && is_space(buf[a])) {
        a++;
    }
    while (b > a && is_space(buf[b - 1u])) {
        b--;
    }
    if (a == b) {
        return HCIP_STAGE_A;                             /* empty file (touch): stage A, as before stage B existed */
    }
    if (b - a == 1u && (buf[a] == 'A' || buf[a] == 'a')) {
        return HCIP_STAGE_A;
    }
    if (b - a == 1u && (buf[a] == 'B' || buf[a] == 'b')) {
        return HCIP_STAGE_B;
    }
    return HCIP_STAGE_NONE;
}

hcip_trig_t hci_probe_trigger_take(exfat_vol_t *vol, int mounted, uint8_t *stage)
{
    exfat_entry_t ent;
    uint8_t buf[HCI_PROBE_TRIGGER_MAX];
    uint32_t got = 0u;
    uint8_t st = HCIP_STAGE_NONE;
    if (stage != NULL) {
        *stage = HCIP_STAGE_NONE;
    }
    if (vol == NULL || !mounted) {
        return HCIP_TRIG_NO_SD;
    }
    if (exfat_find_path(vol, "/" HCI_PROBE_TRIGGER_FILE, &ent) != 0) {
        return HCIP_TRIG_ABSENT;
    }
    if (ent.is_dir) {
        return HCIP_TRIG_NOT_FILE;
    }
    /* The stage is read before the delete; an unreadable or oversized request names no stage (refused below). */
    if (ent.size <= sizeof(buf) &&
        exfat_read_file(vol, &ent, 0u, buf, (uint32_t)ent.size, &got) == 0 && got == (uint32_t)ent.size) {
        st = hci_probe_trigger_parse(buf, got);
    }
    /* Consumed before the probe starts: whatever happens next (hang, fault, WDT cold reset), the next boot finds no
     * request. A delete that fails or does not stick means the request cannot be consumed, so the probe must not run.
     * A request with bad content is consumed too, so it is reported once instead of on every boot. */
    if (exfat_delete_file(vol, "/", HCI_PROBE_TRIGGER_FILE) != 0 ||
        exfat_find_path(vol, "/" HCI_PROBE_TRIGGER_FILE, &ent) == 0) {
        return HCIP_TRIG_STUCK;
    }
    if (st == HCIP_STAGE_NONE) {
        return HCIP_TRIG_BAD;
    }
    if (stage != NULL) {
        *stage = st;
    }
    return HCIP_TRIG_TAKEN;
}

const char *hci_probe_trigger_name(hcip_trig_t t)
{
    switch (t) {
    case HCIP_TRIG_TAKEN:
        return "taken";
    case HCIP_TRIG_ABSENT:
        return "absent";
    case HCIP_TRIG_NO_SD:
        return "no_sd";
    case HCIP_TRIG_NOT_FILE:
        return "not_file";
    case HCIP_TRIG_STUCK:
        return "stuck";
    case HCIP_TRIG_BAD:
        return "bad_content";
    }
    return "?";
}

int hci_probe_qemu_only_refused(uint32_t qemu_only, uint32_t efuse_w3)
{
    return qemu_only != 0u && (efuse_w3 & (1u << 31)) != 0u;
}

int hci_probe_log_save(exfat_vol_t *vol, int mounted, const char *data, uint32_t len, exfat_writer_t *w)
{
    exfat_entry_t ent;
    if (vol == NULL || !mounted || w == NULL || (data == NULL && len != 0u)) {
        return -1;
    }
    if (exfat_find_path(vol, "/" HCI_PROBE_LOG_FILE, &ent) == 0) {
        if (ent.is_dir || exfat_delete_file(vol, "/", HCI_PROBE_LOG_FILE) != 0) {
            return -1;                                   /* never replace a directory; keep an undeletable file */
        }
    }
    if (exfat_create_file(vol, "/", HCI_PROBE_LOG_FILE, w) != 0) {
        return -1;
    }
    if (len != 0u && exfat_write(w, data, len) != 0) {
        (void)exfat_close_file(w);
        return -1;
    }
    return exfat_close_file(w);
}
