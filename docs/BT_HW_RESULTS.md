# Bluetooth plan: hardware results

Silicon results for `docs/BLUETOOTH_BLUEDROID_PLAN.md` (M5 experiments). Device: HW_76 (SL6801), USB-attached,
probes run with `tools/dev_cycle.py exp` on `DEV_EXP=1` images. Production firmware restored after each session.

## Session 1 (2026-11-24)

### Exp 0 — SRAM candidate regions: PASS
Image: default (non-RTOS) `DEV_EXP=1`, which uses neither region. `exp/exp_sram_fill.c` wrote an address-hash pattern
over `0x805C00..0x81A37F` (81.9 KiB) and `0x86B000..0x87FFFF` (84 KiB), 42,464 words.

| Check point | Activity since fill | Changed words |
|---|---|---|
| immediately | — | 0 / 0 |
| after exercise | legacy BT 20 s (`exp_bt.c`: RF cal, RW-BT ROM controller, discoverable, 32,021 IRQ-26 events, HCI ready), DAC DMA playback 2 s (`exp_dac.c`), mic DMA capture 5 s (0 overruns) + 3 SD file writes (`exp_sram_exercise.c`), USB probe traffic, CPU1 LVGL UI | 0 / 0 |

Not yet covered: BT ACL/connection traffic and power-mode transitions — re-check during Exp 1 stage C / Exp 5.
Consequence: the lower region is now heap_5 region 0 in RTOS builds (`sl6801_firm.ld` `RTOS_HEAP_LO`).

### RTOS + BT image self-test on silicon: 14/14 PASS
Image: `DEV_EXP=1 BT=1 RTOS_SELFTEST=1` (Bluedroid linked, not started). Read back with `exp/exp_peek.c`.
delay 50 ms, queue 100, priority-inheritance mutex, timer, notify, event group, heap, preemption 20 ms, 10 FromISR
notifications, stack watermark, self-delete reap, `.bt_data` copy (168 B), malloc failure returns NULL (no reboot),
`bt_port` glue (bt_alarm task delivers a 5 ms esp_timer exactly once after the drain).
Free heap at boot: 34,424 B with the upper region only; **118,256 B** with both regions (min 95,536 B after the
tests), identical to QEMU.

### Exp 2 — noise sources (first read; 16 KiB raw dumps, `exp/exp_entropy_dump.c`)
Quick estimators (MCV with 99 % upper bound, binary Markov, lag-1 correlation, bz2 on packed bits):

| Source | Result |
|---|---|
| mic ADC, gain 8, bits 0/1/2 | ~0.95–0.98 bits per bit each, transition probabilities 0.49–0.51, packed bits incompressible (bz2 ratio 1.19) |
| mic ADC, gain 31, bit 0 | ~0.95; bits 1–2 weaker (0.54–0.78) |
| SARADC key ladder, back-to-back | stuck/correlated (lag-1 r = 0.999) — unusable |
| SARADC key ladder, 200 µs apart | weak (bit-0 Markov 0.28) |
| timer jitter (us timer over a busy loop) | 0.22 bits/byte — unusable |

Provisional source config: mic ADC at gain 8, the 2 low bits of each sample as one sample, credited at **1 bit/sample**
(about half the measured value). At 16 kHz the 384-bit seed needs ~0.1 s after the 1024-sample start-up window.
Still required before crediting in a release: NIST SP 800-90B `ea_non_iid` on longer dumps, cold-start and quiet-room
repeats, and confirmation that the noise is electronic (it is present at low gain).

### Other observations
- eFuse words: `w0=0x1510011D w1=0x003D4000 w2=0x00000600 w3=0xC0000440`; the legacy BD_ADDR `02:CA:55:00:01:1D`
  ends with w0's low bytes — candidate stable device-identity input (plan M1 "Local identity").
- Idle key-ladder ADC 4055..4090.
- One SD multi-block write timed out and fell back to single-sector writes (existing driver behaviour, `SDW_MDTO`).

## Session 2 (2026-11-24): Exp 1 stage A — PASS

Image: `BT=1 HCI_PROBE=1` at `6d2a093`, one-shot request `HCIPROBE.RUN` (consumed: `trig=taken`). `bt_ctrl` drove the real
RW-BT ROM through `bt_rom_real.c`. Result: `END st=DONE fails=0` in 6.3 s; afterwards `legacy=allowed`.

| Item | Result |
|---|---|
| HCI Reset (`bt_ctrl` real reset: RF calibration + ROM start) | CC status 0 in 205 ms (RF cal 202 ms, start 2.2 ms) |
| Read-only commands (answered locally) | 1005, 1001, 1009 (BD_ADDR matches eFuse: `02:CA:55:00:01:1D`), 1002, 1004 p0/p1, 0C14 — all CC status 0, 146–288 µs |
| IRQ 26 liveness after Reset | 3 IRQs within 15 µs |
| Clean restart (stop + start + Reset + Read BD_ADDR) ×5 | 5/5 OK, stop 14–15 µs, no retries — answers BT_ROM_INTERFACE §11 Q9 |
| Idle IRQ 26 (5 s, controller up, no traffic) | only INTSTAT bit 0, 1600.1 Hz (one per 625 µs = 2 slots), no coalescing, 0 spurious |
| IRQ → bottom half latency | avg 8 µs, max 40 µs; `irq_service()` max 41 µs; ROM `run()` max 8 µs; `bt_ctrl_run()` max 80 µs (plan asserts ≤ 1 ms) |
| `bt_ctrl` CPU at idle | **5.3 %** (8000 wake-ups / 5 s) — a power cost to address (only bring the controller up when a mode needs it; look for ROM sleep/low-power modes) |
| NVIC | **4 implemented priority bits (mask 0xF0)**; IRQ 26 set to 0x80 and read back 0x80; SHPR3 0xF0F00000 (PendSV/SysTick lowest). Original IRQ-26 IPR before the probe was **0xF0**, not 0 as BT_ROM_INTERFACE §12 assumed; restored on release. |
| `g_bt_heap` (8 KiB ROM heap) | high-water 0 in stage A (no ROM messages yet: stage B/C will load it) |
| Free stack | bt_ctrl 888 B, bt_dlv 1372 B, probe 1196 B |
| RTOS heap free during probe | 104,720 B |

Follow-ups: `FreeRTOSConfig.h` now states `configPRIO_BITS 4` with the syscall ceiling kept at 0x80 (image unchanged: the
V11 port reads the implemented bits from hardware). Stage B (verified writes 0C56/0C01/0C24/0C13/0C1A) and the
"needs disassembly" rows are next; ACL/connection traffic (stage C) also repeats the Exp 0 memory check.

## Exp 1 stage A: how to run

Probe image for plan M5 Exp 1 stage A (step 4): `bt_ctrl` (plan M1) drives the real RW-BT ROM through
`open_fw/bt_port/bt_rom_real.c` with a host-side driver stub instead of Bluedroid (`open_fw/bt_port/hci_probe.c`).
No results yet; record them in a new session section below when the probe has run.

**When it runs:** only on request, once. At boot (after the UI and SD are up) the image looks for an empty file
`HCIPROBE.RUN` in the microSD root; it deletes it, checks that the delete stuck, and only then starts the probe. Without
the file the image behaves like a plain `BT=1` image and logs `HCIP OFF trig=absent`. A probe that hangs or resets the
device therefore cannot run again on the next boot unless you create the file again.

**What it does** (~15 s, then BT stays off; the app keeps feeding the WDT):
1. HCI Reset `0C03` = `bt_ctrl`'s real reset (RF calibration with the legacy `bt_rf_init_calibrated(0x53, 32 MHz)`,
   ROM start reusing the legacy LD/LC/LM bring-up, IRQ 26 at NVIC priority 0x80), then IRQ-26 liveness (an IRQ within
   500 ms).
2. Read-only commands `bt_ctrl` answers locally (opcode table action SYN): `1005`, `1001`, `1009` (compared with the
   programmed BD_ADDR), `1002`, `1004` pages 0 and 1, `0C14`. No read-only row of the table has a ROM mapping the legacy
   firmware exercises on silicon, so stage A sends **no** KE message to an LM/LC task. Rows that need hardware or
   disassembly are logged as `NOT_PROBED` (G-x: "needs disassembly", V-B/V-C: later stages) and never sent; so is the
   `1009` `E05D→E0C0` cross-check and the `1001` `LMP_version_res` comparison.
3. 5 s idle window: IRQ-26 per-bit counts (top half) vs bottom-half services and re-latches (coalescing), IRQ→bottom-half
   latency (max/avg, `us_timer`), longest `irq_service()`, ROM dispatch and `run()` call, `bt_ctrl` CPU time.
4. Clean restart ×5: teardown (`stop()`) → open → Reset (`start()`) → liveness → `1009`.
5. Summary: NVIC implemented priority bits (`0xFF` written to IRQ 26's IPR byte while masked, read back) and the IRQ-26
   priority read-back, `g_bt_heap` high-water, RF-cal / start / stop durations, ROM message counters, stack watermarks.

Per command: 500 ms deadline (Reset 2 s: it includes the RF calibration), expected initial reply (CC/CS and length from
the table row), raw reply (long replies: first 16 bytes + CRC-32), latency in µs measured at the host callback.

**Safety:** first failure (timeout, bad reply, `bt_ctrl` fault, no IRQ 26) → controlled `bt_ctrl` teardown and one
retry; second failure → probe stops (`END st=ABORT`). A teardown that does not finish in 1 s latches `bt_stop` and masks
IRQ 26 (plan M1 hard path). A `bt_ctrl_run()` stuck for more than 3 s (ROM busy-wait) is demoted to idle priority by a
FreeRTOS timer, so the UI stays usable (`HCIP HANG`).

- *Reset during the probe.* A ROM spin with PRIMASK held, or an IRQ-26 storm, starves the timer and the WDT feed: after
  32 s the runtime WDT does a **PMU cold reset**, which clears `AON_BACKUP(0..15)` (`docs/SL6801_HW76_HARDWARE.md`
  §5.2), so no fault record survives it and the 3-strike counter restarts. What stops a reset loop is the one-shot
  request: it was deleted before the probe started, so the next boot logs `HCIP OFF trig=absent` and boots normally.
  The probe also writes AON fault record `0xFA0170B2` while it runs; only a reset through `system_reboot()` (HardFault,
  `configASSERT`, stack overflow) keeps it, and that boot shows `PREV_FAULT=0xFA0170B2` (`dev_cycle.py` prints
  "previous fault"). If a boot with a fresh request still has a fault record from the previous boot, the probe logs
  `HCIP SKIP` instead of running (the request is consumed; create it again to retry). So `HCIP OFF` right after you
  created the request means that boot ran the probe and the device reset during it.
- *The app while it runs* (`BEGIN` → `END`): each Reset busy-waits ~0.3–0.5 s in RF calibration at `bt_ctrl` priority,
  which the 512 ms mic DMA ring cannot absorb, so starting a recording is refused ("start failed") and USB disk mode
  (including the automatic one) waits for `END`. Leave the device alone until it enumerates over USB again.
- *After it ends.* `stop()` puts IRQ 26's NVIC priority back to the value found before the first `start()` (legacy never
  sets one), and legacy Bluetooth mode stays refused for the rest of the boot if RW-BT may be in an unknown state: from
  the first RF calibration until a `stop()` whose soft reset completed, and after `HANG` / `HARDSTOP`. `HCIP RELEASE`
  logs both (`ipr`, `orig`, `claimed`, `legacy=allowed|refused`); reboot before using BT mode after a `refused`.
- Do not run `dev_cycle.py exp` experiments during the probe window.

**Run:**
```bash
# 1. Build (update-up also writes dumps/firmware_2M_openfw.bin, which flash uses: run it right before flashing; the
#    test-qemu-* targets overwrite that file with QEMU-only images).
B=/tmp/sl6801_openfw_hciprobe_hw
make -C open_fw BT=1 HCI_PROBE=1 BUILD_DIR=$B update-up        # check "Plan: ..." ~737 KB (BT image)
# 2. Flash. No request file yet, so this boot logs "HCIP OFF" and USB comes up as usual.
sudo env OPENFW_FIRM_BIN=$B/open_fw_firm.bin python3 tools/dev_cycle.py flash
# 3. Create the one-shot request on the microSD (device in USB disk mode). Find the exFAT partition with lsblk.
lsblk -o NAME,MODEL,FSTYPE,SIZE,MOUNTPOINT
udisksctl mount -b /dev/sdX1                                    # if not mounted already; prints the mount point
touch /media/$USER/<label>/HCIPROBE.RUN && sync
udisksctl unmount -b /dev/sdX1
# 4. Reboot into the probe. USB enumerates only after HCIP END (~15-25 s after the home screen), hence the timeout.
sudo python3 tools/dev_cycle.py dfu
sudo python3 tools/dev_cycle.py boot --timeout 60
# 5. Read and report.
sudo python3 tools/dev_cycle.py log > /tmp/hcip.log
python3 tools/bt_probe_report.py /tmp/hcip.log --md /tmp/hcip.md --json /tmp/hcip.json
```
Check that `/tmp/hcip.log` has `trig=taken` in `BEGIN` (the report warns on `trig=autorun`: a QEMU-only image). Then
re-flash production firmware (`make -C open_fw all update-up && sudo python3 tools/dev_cycle.py flash`).

**Reading the log:** one line per result, prefix `HCIP ` (`OFF`, `BEGIN`, `NOT_PROBED`, `CMD`, `LIVE`/`TIMEOUT`, `FAIL`,
`TEARDOWN`, `NVIC`, `IDLE`, `IRQBITS` `b<bit>=<occurrences>/<serviced>/<latched while pending>`, `RESTART`, `HEAP`,
`TIME`, `CTRL`, `STACK`, `RELEASE`, `END st=DONE|ABORT|SKIP`). The probe writes ~2.7 KB into the 4 KiB `g_openfw_log` ring: read
the log before doing anything else that logs. The report warns when `BEGIN` or `END` is missing.

**QEMU** (`make -C open_fw test-qemu-hciprobe`): no RW-BT model, so the expected result is Reset CC → `TIMEOUT
what=irq26` → teardown → retry → `RELEASE ... legacy=allowed` → `END st=ABORT`, with the normal boot checks and the
RTOS self-test passing. The QEMU SD card is blank, so both QEMU targets build with `HCI_PROBE_AUTORUN=1` (`trig=autorun`;
the request-file logic is host-tested in `make -C open_fw test-bt-port`, `tests/test_hci_probe.c`).
`make -C open_fw test-qemu-hciprobe-full` is a dev check of the whole sequence with the liveness check waived
(`waived=1`); QEMU timings are not meaningful. Neither image they leave in `dumps/` may be flashed.
