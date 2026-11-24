/* hci_probe_trigger: one-shot SD request file for the HCI probe (see hci_probe_trigger.h).
 * SPDX-License-Identifier: MIT */
#include <stddef.h>
#include "hci_probe_trigger.h"

hcip_trig_t hci_probe_trigger_take(exfat_vol_t *vol, int mounted)
{
    exfat_entry_t ent;
    if (vol == NULL || !mounted) {
        return HCIP_TRIG_NO_SD;
    }
    if (exfat_find_path(vol, "/" HCI_PROBE_TRIGGER_FILE, &ent) != 0) {
        return HCIP_TRIG_ABSENT;
    }
    if (ent.is_dir) {
        return HCIP_TRIG_NOT_FILE;
    }
    /* Consumed before the probe starts: whatever happens next (hang, fault, WDT cold reset), the next boot finds no
     * request. A delete that fails or does not stick means the request cannot be consumed, so the probe must not run. */
    if (exfat_delete_file(vol, "/", HCI_PROBE_TRIGGER_FILE) != 0 ||
        exfat_find_path(vol, "/" HCI_PROBE_TRIGGER_FILE, &ent) == 0) {
        return HCIP_TRIG_STUCK;
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
    }
    return "?";
}
