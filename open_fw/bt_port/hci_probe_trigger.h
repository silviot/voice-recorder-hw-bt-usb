/* hci_probe_trigger: the HCI probe (hci_probe.c, plan M5 Exp 1) runs only when the user asked for it, once.
 * The request is an SD-card file in the root directory; it is deleted (and the deletion verified) before the probe
 * touches the controller, so a probe that hangs or resets the device (a 32 s WDT PMU cold reset clears AON_BACKUP,
 * docs/SL6801_HW76_HARDWARE.md §5.2) cannot run again on the next boot without the user creating the file again.
 * The file's content selects the stage: empty or "A" = stage A; "B" = stage A, then stage B (verified writes). Case,
 * surrounding whitespace / line ends and a UTF-8 BOM are ignored; anything else is consumed and refused.
 * The probe's own log copy is saved next to it (HCI_PROBE_LOG_FILE) by the app task after the probe ends.
 * Pure exFAT logic: host-tested in tests/test_hci_probe.c (make test-bt-port).
 * SPDX-License-Identifier: MIT */
#ifndef HCI_PROBE_TRIGGER_H
#define HCI_PROBE_TRIGGER_H

#include <stdint.h>
#include "openfw_fs.h"

#define HCI_PROBE_TRIGGER_FILE "HCIPROBE.RUN"   /* SD root; exFAT names are case-insensitive */
#define HCI_PROBE_LOG_FILE     "HCIPROBE.LOG"   /* SD root; every HCIP line of the last run (replaced each run) */
#define HCI_PROBE_TRIGGER_MAX  32u              /* longer request files are refused (not a stage word) */

typedef enum {
    HCIP_TRIG_TAKEN = 0,   /* the file was there and is gone now: run the probe once */
    HCIP_TRIG_ABSENT,      /* no request: do not run */
    HCIP_TRIG_NO_SD,       /* no mounted volume: do not run */
    HCIP_TRIG_NOT_FILE,    /* the name exists but is a directory: do not run */
    HCIP_TRIG_STUCK,       /* the file could not be deleted: do not run (it would run again on every boot) */
    HCIP_TRIG_BAD,         /* consumed, but its content names no stage (or could not be read): do not run */
} hcip_trig_t;

enum { HCIP_STAGE_NONE = 0, HCIP_STAGE_A = 1, HCIP_STAGE_B = 2 };

/* Stage named by a request file's content (HCIP_STAGE_A / _B), HCIP_STAGE_NONE if it names none. */
uint8_t hci_probe_trigger_parse(const uint8_t *buf, uint32_t len);

/* Check for the request file, read its stage and consume it. Call from the task that owns the filesystem (the app
 * task). *stage is set for HCIP_TRIG_TAKEN only (HCIP_STAGE_NONE otherwise). */
hcip_trig_t hci_probe_trigger_take(exfat_vol_t *vol, int mounted, uint8_t *stage);

/* Short word for the log ("taken", "absent", "no_sd", "not_file", "stuck", "bad_content"). */
const char *hci_probe_trigger_name(hcip_trig_t t);

/* QEMU-only probe images (HCI_PROBE_AUTORUN: runs without the request, every boot; HCI_PROBE_WAIVE_LIVENESS: waives
 * the liveness check and stage B's reply timeouts) must not run on silicon. QEMU's SL6801 model maps no eFuse
 * controller (EFUSE_WORD reads 0, so the probe logs src=fixed there), while a programmed part has the eFuse valid bit
 * (word 3 bit 31) set (HW_76 unit: w3 = 0xC0000440). Returns 1 = refuse: qemu_only != 0 and efuse_w3 bit 31 set.
 * Fails open on a part whose eFuse is not programmed; the build's other half is that these images never reach
 * dumps/ (open_fw/Makefile update-up refuses, the QEMU targets write their own 2 MB image). */
int hci_probe_qemu_only_refused(uint32_t qemu_only, uint32_t efuse_w3);

/* Replace HCI_PROBE_LOG_FILE in the SD root with data[0..len). App task only; w is caller scratch (keeps the 0.5 KiB
 * sector buffer off the app stack). Returns 0 on success, -1 otherwise (no volume, read-only, write error). */
int hci_probe_log_save(exfat_vol_t *vol, int mounted, const char *data, uint32_t len, exfat_writer_t *w);

#endif /* HCI_PROBE_TRIGGER_H */
