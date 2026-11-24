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

## Exp 1 stages A and B: how to run

Probe image for plan M5 Exp 1 stage A and stage B (step 4): `bt_ctrl` (plan M1) drives the real RW-BT ROM through
`open_fw/bt_port/bt_rom_real.c` with a host-side driver stub instead of Bluedroid (`open_fw/bt_port/hci_probe.c`).
Stage A results: session 2 above. Record new results in a new session section above this one.

**When it runs:** only on request, once. At boot (after the UI and SD are up) the image looks for a file
`HCIPROBE.RUN` in the microSD root; it reads it, deletes it, checks that the delete stuck, and only then starts the probe.
The file's content picks the stage:

| `HCIPROBE.RUN` content | Runs |
|---|---|
| empty (`touch`) or `A` | stage A (6.3 s on silicon, session 2) |
| `B` | stage A, then stage B (~33 s more: 30 s of it is the discoverable window) |
| anything else | nothing: the file is consumed and the boot logs `HCIP OFF trig=bad_content` |

Case, surrounding spaces/line ends and a UTF-8 BOM are ignored (`echo B >` and files saved by Windows Notepad both work). Without the file
the image behaves like a plain `BT=1` image and logs `HCIP OFF trig=absent`. A probe that hangs or resets the device
therefore cannot run again on the next boot unless you create the file again.

**Stage A** (BT stays off afterwards unless stage B follows; the app keeps feeding the WDT):
1. HCI Reset `0C03` = `bt_ctrl`'s real reset (RF calibration with the legacy `bt_rf_init_calibrated(0x53, 32 MHz)`,
   ROM start reusing the legacy LD/LC/LM bring-up, IRQ 26 at NVIC priority 0x80), then IRQ-26 liveness (an IRQ within
   500 ms).
2. Read-only commands `bt_ctrl` answers locally (opcode table action SYN): `1005`, `1001`, `1009` (compared with the
   programmed BD_ADDR), `1002`, `1004` pages 0 and 1, `0C14`. No read-only row of the table has a ROM mapping the legacy
   firmware exercises on silicon, so stage A sends **no** KE message to an LM/LC task. Rows that need hardware or
   disassembly are logged as `NOT_PROBED` (G-x: "needs disassembly", V-B/V-C: later stages; the V-B line is left out
   when stage B runs) and never sent; so is the `1009` `E05D→E0C0` cross-check and the `1001` `LMP_version_res`
   comparison.
3. 5 s idle window: IRQ-26 per-bit counts (top half) vs bottom-half services and re-latches (coalescing), IRQ→bottom-half
   latency (max/avg, `us_timer`), longest `irq_service()`, ROM dispatch and `run()` call, `bt_ctrl` CPU time.
4. Clean restart ×5: teardown (`stop()`) → open → Reset (`start()`) → liveness → `1009`.
5. Summary: NVIC implemented priority bits (`0xFF` written to IRQ 26's IPR byte while masked, read back) and the IRQ-26
   priority read-back, `g_bt_heap` high-water, RF-cal / start / stop durations, ROM message counters, stack watermarks.

**Stage B** (plan M5 "verified writes"), on the session the 5th restart left up: the opcode-table **V-B** rows, i.e. the
FWD writes whose ROM KE ids and payload layouts the legacy glue uses on silicon (`bt_hci.c` F:882–904 request ids and
sizes, F:742–747 reply packers), with the parameters Bluedroid or the legacy firmware send. Order and checks:

| # | Command (parameter) | Expected | Read-back |
|---|---|---|---|
| 1 | `0C56` Write Simple Pairing Mode (`01`) | `CC(4)` s=00 via `E04F→E0B1` | `1004` page 1: features[0] = `01` (`bt_ctrl` sets the SSP host bit only on the ROM's s=00) |
| 2 | `0C01` Set Event Mask (Bluedroid's `FF FF FF FF FF FF BF 1D`, `controller.c:477`, BLE off) | `CC(4)` via `E01C→E079` | `hci_tl_env.evt_msk` (`0x00803D04`) = mask OR mandatory = `FF`×8 |
| 3 | `0C24` Write Class of Device (`04 04 20`, legacy F:1550) | `CC(4)` via `E034→E094` | none (`0C23` is not in the table) |
| 4 | `0C13` Write Local Name (`Cassini-Probe-XXXX`, XXXX = last two BD_ADDR bytes, zero-padded to 248) | `CC(4)` via `E025→E083` | `0C14` returns the same 248 bytes (`bt_ctrl`'s name cache, opcode table §3.7) |
| 5 | `0C1A` Write Scan Enable (`03`: inquiry + page scan) | `CC(4)` via `E02C→E08A` | **30 s discoverable window** (`WIN w=scan`): IRQ-26 per-bit counts and rate, `bt_ctrl` CPU, unsolicited events, ROM messages, `g_bt_heap` high-water / in use |
| 6 | `0C1A` (`00`) | `CC(4)` | 2 s window (`WIN w=off`): the IRQ load must fall back to the idle level |
| 7 | `0C01` zero mask, then Bluedroid's mask again | `CC(4)` each | `evt_msk` = exactly the mandatory bits `00 60 04 00 F8 07 40 E2`, then `FF`×8 |

Step 7 is the only way to tell "the ROM handler passed the parameter to hook `0x00803BBC[7]`" from "nothing was
written" (Bluedroid's mask OR the mandatory bits is all `FF`, the same as the reset default); it runs last so that a
failure there costs nothing else (BT_ROM_INTERFACE §11 Q8). None of these rows has a terminal event: whatever event
arrives within 50 ms of a reply is logged as `EVT` right after its `CMD` line. Stage B `CMD` lines add `fwd=` (1 =
`bt_ctrl` forwarded it to the ROM, not answered locally), `romd=` (ROM messages to TASK_HCI since the send; 1 = the
reply), `hw=` (`g_bt_heap` high-water) and `ctrl_free=` (bytes of the `bt_ctrl` stack never used so far: the ROM's
LM/LC handlers run on it, so the trend per forwarded command shows which one deepened it). `WIN` lines add `cpu_pk=`
(highest `bt_ctrl` CPU share in one 100 ms sample, permille) and `ctrl_free=`. A read-back that does not match is counted (`STAGE ... vfail=`, `END ...
vfail=`) and reported, but does not stop the probe; a bad, missing or late reply does.

*Optional check from a phone during the 30 s window:* a Bluetooth scan should list `Cassini-Probe-XXXX` (the name the
ROM answers to a remote name request: the only check of the name **in the ROM**, since `0C14` reads `bt_ctrl`'s cache)
with an audio device icon (CoD). Do not pair or connect: stage B has no host to answer a connection request (the ROM
rejects it after its accept timeout; it would show up as an `EVT` line).

Per command: 500 ms deadline (Reset 2 s: it includes the RF calibration), expected initial reply (CC/CS and length from
the table row), raw reply (long replies: first 16 bytes + CRC-32), latency in µs measured at the host callback.

**Safety:** stage A: first failure (timeout, bad reply, `bt_ctrl` fault, no IRQ 26) → controlled `bt_ctrl` teardown and
one retry; second failure → probe stops (`END st=ABORT`). Stage B: the first failure stops it (`STAGE s=B st=FAIL`,
teardown, `END st=ABORT`); a retry would only repeat the same write. Class-M rows with a nonzero ROM status are a
`bt_ctrl` invariant fault (opcode table §2 rule 7), so they fail too. The windows end early on a `bt_ctrl` fault, hang or
CPU-guard trip (`WIN ... end=fault|hang|cpu`).
A controlled teardown is the ROM soft reset (`stop()`), so it also ends discoverability even if `0C1A 00` was never
answered. The hard paths never complete `stop()`: a teardown that does not finish in 1 s latches `bt_stop` and masks
IRQ 26 (plan M1 hard path, `HCIP HARDSTOP`), and a `bt_ctrl_run()` stuck for more than 3 s (ROM busy-wait) is demoted
to idle priority by a FreeRTOS timer so the UI stays usable (`HCIP HANG why=run`). Both then run
`bt_rom_real_silence()`: `stop()`'s register sequence (IRQ 26 off, `INTCNTL=0`, `INTACK` all, RWBTEN off, soft reset +
bounded poll) without any ROM call, logged as `cntl=` (`RWBT_CNTL` read back: bit 8 RWBTEN must be 0, bit 29 = soft
reset still pending). It is the sequence the legacy `bt_xrec_stop()` uses on silicon, but it has never run while a ROM
call is stuck, and a ROM read-modify-write interrupted before it could set RWBTEN again: **after `HANG` or `HARDSTOP`
in stage B, reboot the device at once** (`sudo python3 tools/dev_cycle.py dfu`, then `boot`, as soon as USB is back:
entering DFU resets the chip; if USB does not come back, hold ON/OFF for the PMU long-press power-off, 6–12 s); do not
assume the device stopped being discoverable. RW-BT stays claimed, so legacy BT mode is refused until that reboot anyway.
*CPU-share guard (stage B only):* stage B is the first time the radio is active (inquiry + page scan), and no `bt_ctrl`
CPU or IRQ-26 rate has been measured on silicon in that state. If an IRQ-26 source re-latches at once (a status bit the
top half does not clear, a flood of scan-slot IRQs), `bt_ctrl` (priority 8) runs back to back and starves the app
(priority 1): no WDT feed, and after 32 s a PMU cold reset loses the whole run with no log. So from its first command to
its end, stage B also samples `bt_ctrl`'s CPU share every 100 ms in the timer daemon (priority 9): above 70 % in 10
consecutive samples (1 s) is treated like a hang (`HCIP HANG why=cpu cpu_pk=...`, `bt_stop`, IRQ 26 masked, demoted,
radio stopped as above). Stage A does not arm it: each HCI Reset keeps `bt_ctrl` busy ~0.2 s (RF calibration), five of
them back to back in the restart loop. Neither guard helps against a ROM spin with PRIMASK set or a storm that keeps
the CPU inside the IRQ-26 top half itself: the WDT then resets the device (next bullet).

- *Reset during the probe.* A ROM spin with PRIMASK held, or an IRQ-26 storm, starves the timer and the WDT feed: after
  32 s the runtime WDT does a **PMU cold reset**, which clears `AON_BACKUP(0..15)` (`docs/SL6801_HW76_HARDWARE.md`
  §5.2), so no fault record survives it and the 3-strike counter restarts. What stops a reset loop is the one-shot
  request: it was deleted before the probe started, so the next boot logs `HCIP OFF trig=absent` and boots normally.
  The probe also writes AON fault record `0xFA0170B2` while it runs; only a reset through `system_reboot()` (HardFault,
  `configASSERT`, stack overflow) keeps it, and that boot shows `PREV_FAULT=0xFA0170B2` (`dev_cycle.py` prints
  "previous fault") or, when the fault handler overwrote it, the fault's own record: a FreeRTOS stack overflow is
  `PREV_FAULT=0xFA0170F2 PC=<TCB address> LR=<task name address>` (`openfw_rtos_fatal(0xF2, task, name)`; look the TCB
  address up with `llvm-nm` on the probe ELF: `s_ctrl_tcb` = `bt_ctrl`). The HCIP log of the crashed run itself is lost
  (the ring and the SRAM_HI copy are `.bss`, zeroed at boot), so per-command `ctrl_free=` only helps runs that finish;
  the probe image therefore gives `bt_ctrl` a 4 KiB stack instead of the plan's 2 KiB (stage A used 1.2 KiB idle) and
  logs `STACK ctrl_size=` so the real depth is still measured. If a boot with a fresh request still has a fault record from the previous boot, the probe logs
  `HCIP SKIP` instead of running (the request is consumed; create it again to retry). So `HCIP OFF` right after you
  created the request means that boot ran the probe and the device reset during it.
- *The app while it runs* (`BEGIN` → `LOGFILE`): each Reset busy-waits ~0.3–0.5 s in RF calibration at `bt_ctrl`
  priority, which the 512 ms mic DMA ring cannot absorb, so starting a recording is refused ("start failed") and USB disk
  mode (including the automatic one) waits until the probe has ended and written its log file. Leave the device alone
  until it enumerates over USB again.
- *After it ends.* `stop()` puts IRQ 26's NVIC priority back to the value found before the first `start()` (legacy never
  sets one), and legacy Bluetooth mode stays refused for the rest of the boot if RW-BT may be in an unknown state: from
  the first RF calibration until a `stop()` whose soft reset completed, and after `HANG` / `HARDSTOP`. `HCIP RELEASE`
  logs both (`ipr`, `orig`, `claimed`, `legacy=allowed|refused`); reboot before using BT mode after a `refused`.
- Do not run `dev_cycle.py exp` experiments during the probe window.

**Run:**
```bash
# 1. Build (update-up also writes dumps/firmware_2M_openfw.bin, which is what flash writes: OPENFW_FIRM_BIN only gives
#    the length. Run it right before flashing; test-qemu-rtos / test-qemu-bt overwrite that file with self-test images.
#    The QEMU-only probe targets write their own BUILD_DIR, and update-up refuses HCI_PROBE_AUTORUN / _WAIVE_ builds
#    aimed at dumps/).
B=/tmp/sl6801_openfw_hciprobe_hw
make -C open_fw BT=1 HCI_PROBE=1 BUILD_DIR=$B update-up        # check "Plan: ..." ~741 KB (BT image)
# 2. Flash. No request file yet, so this boot logs "HCIP OFF" and USB comes up as usual.
sudo env OPENFW_FIRM_BIN=$B/open_fw_firm.bin python3 tools/dev_cycle.py flash
# 3. Create the one-shot request on the microSD (device in USB disk mode). Find the exFAT partition with lsblk.
lsblk -o NAME,MODEL,FSTYPE,SIZE,MOUNTPOINT
udisksctl mount -b /dev/sdX1                                    # if not mounted already; prints the mount point
M=/media/$USER/<label>
echo B > $M/HCIPROBE.RUN && sync                                # stage A + B; `touch` (empty) = stage A only
udisksctl unmount -b /dev/sdX1
# 4. Reboot into the probe. USB enumerates only after the probe wrote HCIPROBE.LOG (stage A ~10-25 s, stage A + B
#    ~40-60 s after the home screen), hence the timeout.
sudo python3 tools/dev_cycle.py dfu
sudo python3 tools/dev_cycle.py boot --timeout 120
# 5. Read and report. Stage A + B write ~4.5 KB of HCIP lines, more than the 4 KiB g_openfw_log ring holds, so the
#    complete run is in SD:/HCIPROBE.LOG; the ring keeps the tail (and anything logged after the probe).
sudo python3 tools/dev_cycle.py log > /tmp/hcip_ring.log           # read the ring first, before anything else logs
udisksctl mount -b /dev/sdX1 && cp $M/HCIPROBE.LOG /tmp/hcip.log && udisksctl unmount -b /dev/sdX1
python3 tools/bt_probe_report.py /tmp/hcip.log --md /tmp/hcip.md --json /tmp/hcip.json
```
Check that `/tmp/hcip.log` has `trig=taken` and the stage you asked for (`stage=A|B`) in `BEGIN` (the report warns on
`trig=autorun` / `waive_live`: QEMU-only images), and that the ring log ends with `HCIP LOGFILE ... st=OK`
(`st=FAIL`: no file was written; the ring then holds only the last ~4 KB, so a stage A run is still complete there, but
a stage B run has lost `BEGIN`, `NOT_PROBED` and the first stage A lines. The `STAGE s=B` start line repeats `trig=`
and the stage, and the report falls back on it to identify the run; the stage A reads must then be re-run).
`HCIPROBE.LOG` is replaced by every run. Then re-flash production firmware
(`make -C open_fw all update-up && sudo python3 tools/dev_cycle.py flash`).

**Reading the log:** one line per result, prefix `HCIP ` (`OFF`, `BEGIN`, `NOT_PROBED`, `CMD`, `LIVE`/`TIMEOUT`, `FAIL`,
`TEARDOWN`, `NVIC`, `IDLE`, `IRQBITS` `b<bit>=<occurrences>/<serviced>/<latched while pending>`, `RESTART`; stage B:
`STAGE s=B` (start: name, window, `trig=`, CPU-guard limits `cpu_pm= cpu_n=`; end: `st=DONE|FAIL vfail=`),
`VERIFY what=ssp|evtmask|name ... res=match|MISMATCH`, `WIN w=scan|off` and `WINBITS` (same per-bit format); on the
hard paths `HARDSTOP` / `HANG why=run|cpu` with `cntl=`; then `HEAP`, `TIME`, `CTRL`, `STACK` (`ctrl_size` /
`ctrl_free`: used = size - free), `RELEASE`,
`END st=DONE|ABORT|SKIP`, and `LOGFILE` once the app task has written `HCIPROBE.LOG`). Stage A alone writes ~2.7 KB;
`HCIPROBE.LOG` keeps up to 8 KiB (`LOGFILE lost=` counts lines that did not fit; QEMU's stage A + B is ~5.2 KB). The report compares the idle, scan and
scan-off windows side by side (rate, CPU, per-sample CPU peak, stack, per-bit Hz) and warns on missing `BEGIN`/`END`,
read-back mismatches, a window that ended early (fault, hang, CPU guard), a `HANG` / `HARDSTOP` (reboot now), a
QEMU-only image, and a failed log file.

**QEMU** (`make -C open_fw test-qemu-hciprobe`): no RW-BT model, so the expected result is Reset CC → `TIMEOUT
what=irq26` → teardown → retry → `RELEASE ... legacy=allowed` → `END st=ABORT` → `LOGFILE ... st=OK`, with the normal
boot checks and the RTOS self-test passing. The QEMU SD card is blank, so both QEMU targets build with
`HCI_PROBE_AUTORUN` (`trig=autorun`; 1 = stage A, 2 = stage A + B; the request-file logic is host-tested in
`make -C open_fw test-bt-port`, `tests/test_hci_probe.c`). `make -C open_fw test-qemu-hciprobe-full` is a dev check of
the whole stage A + B sequence and its log volume with the liveness check and stage B's ROM replies waived (`waived=1`)
and a 3 s scan window; QEMU timings and stage B read-backs (all `MISMATCH` there) are not meaningful. Neither image may
be flashed, and two layers keep them off the device: both targets write their 2 MB image into their own `BUILD_DIR`
(`firmware_2M_qemu_only.bin`; `update-up` refuses an `HCI_PROBE_AUTORUN` / `HCI_PROBE_WAIVE_LIVENESS` build whose
`OUT_BIN` or `OUT_UP` is in `dumps/`), and such an image checks the eFuse valid bit at boot and logs
`HCIP OFF trig=qemu_only_on_silicon` instead of running when it is set (QEMU maps no eFuse controller: it reads 0).
The eFuse check fails open on a part whose eFuse is not programmed.

Lessons from building stage B (QEMU, 2026-11-24):
- **QEMU's ROM never answers a forwarded command.** The KE message is allocated from `g_bt_heap` (`hw=32` for `0C56`)
  and sent, but no reply reaches TASK_HCI (`romd=0`): QEMU cannot check any FWD row, only silicon can.
- **One unanswered FWD command wedges the whole command path until teardown.** `bt_ctrl` admits one command at a time to
  the ROM (`BT_CTRL_ROM_CMD_CREDITS = 1`) and handles host commands in order, so after it every later command — SYN reads
  included — times out (`fwd=0`). Teardown clears it (stop with a FWD pending: `st=OK`). On silicon a lost ROM reply
  would therefore stall Bluedroid's command queue, not just one command; stage B fails fast instead of retrying.
- **Stage A + B overflow the 4 KiB `g_openfw_log` ring** (stage A ~2.7 KB on silicon, ~3 KB in QEMU with its waived
  `LIVE` lines; QEMU stage A + B 4.96 KB), losing `BEGIN`: hence the SRAM_HI copy written to `HCIPROBE.LOG` by the app task
  (the filesystem owner) after `END`, with USB MSC held off until it is written.
- **`rom_rst_release(0x58)` / `rom_clk_enable(0x1B)` are the eFuse block's reset and clock** (`bt_rf.c`
  `EFUSE_RST_ID` / `EFUSE_CLK_ID`, also `audio_dac.c`), not RW-BT's: `probe_bd_addr()` and the legacy
  `derive_local_bd_addr_and_name()` call them only to read the eFuse words. Their inverse would not stop the radio. The
  BT/RF block's own controls are `CRU_RST_BT` (0x22) / `CRU_RST_SET1` bit 2 / `CRU_CLK_SRC_GATE` bit 4, which the legacy
  code only pulses inside `rf_cru_reset()` and never holds off; holding them off while a ROM call may still touch RW-BT
  registers is untested (a peripheral in reset or without clock can stall the bus on some SoCs), so the hard path uses
  the legacy stop's RW-BT register sequence instead (`bt_rom_real_silence()`).
- **QEMU's SL6801 model maps no eFuse controller and no RW-BT** (the `0x30000000..0x5FFFFFFF` unimplemented device reads
  0) and keeps no `AON_BACKUP` values (its `0x40085000` model returns 0 for them), so neither an AON marker set by the
  boot stub nor RW-BT's version register can tell QEMU from silicon; the eFuse valid bit (word 3 bit 31) can, on a
  programmed part.
- **A CPU-share check must read the busy total and the "run in progress" flag atomically.** `bt_ctrl_rtos.c` clears
  `s_in_run` inside the same critical section that adds the run to `ctrl_run_total_us`; clearing it first (as before)
  let a timer-daemon sample in between see the run counted zero times, which the unsigned difference turns into a huge
  busy delta (clamped to 100 %): a false trip. `bt_cpu_guard_step()` also treats a backwards busy step as 0 %.
- `test-qemu-hciprobe-full` once reported the RTOS self-test `btport` failing at step 3 (the 5 ms esp_timer had already
  fired after a 3 ms delay) and passed on the next two runs: the self-test's timing check races the probe's first Reset
  (RF-calibration busy-wait at `bt_ctrl` priority) under QEMU. Re-run before suspecting the port. The same `btport`
  step-3 failure also showed up once in plain `test-qemu-bt` (no probe linked; the next three runs passed) while the
  host was building in parallel: the check depends on QEMU's wall-clock pacing, not only on the probe.
