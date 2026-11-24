/* hci_probe_trigger: the HCI probe (hci_probe.c, plan M5 Exp 1 stage A) runs only when the user asked for it, once.
 * The request is an SD-card file in the root directory; it is deleted (and the deletion verified) before the probe
 * touches the controller, so a probe that hangs or resets the device (a 32 s WDT PMU cold reset clears AON_BACKUP,
 * docs/SL6801_HW76_HARDWARE.md §5.2) cannot run again on the next boot without the user creating the file again.
 * Pure exFAT logic: host-tested in tests/test_hci_probe_trigger.c (make test-bt-port).
 * SPDX-License-Identifier: MIT */
#ifndef HCI_PROBE_TRIGGER_H
#define HCI_PROBE_TRIGGER_H

#include "openfw_fs.h"

#define HCI_PROBE_TRIGGER_FILE "HCIPROBE.RUN"   /* SD root; contents ignored; exFAT names are case-insensitive */

typedef enum {
    HCIP_TRIG_TAKEN = 0,   /* the file was there and is gone now: run the probe once */
    HCIP_TRIG_ABSENT,      /* no request: do not run */
    HCIP_TRIG_NO_SD,       /* no mounted volume: do not run */
    HCIP_TRIG_NOT_FILE,    /* the name exists but is a directory: do not run */
    HCIP_TRIG_STUCK,       /* the file could not be deleted: do not run (it would run again on every boot) */
} hcip_trig_t;

/* Check for the request file and consume it. Call from the task that owns the filesystem (the app task). */
hcip_trig_t hci_probe_trigger_take(exfat_vol_t *vol, int mounted);

/* Short word for the log ("taken", "absent", "no_sd", "not_file", "stuck"). */
const char *hci_probe_trigger_name(hcip_trig_t t);

#endif /* HCI_PROBE_TRIGGER_H */
