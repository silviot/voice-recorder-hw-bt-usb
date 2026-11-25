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

**Wired (2026-11-24, BT=1 phone images): `open_fw/bt_port/bt_entropy_mic.c`, credited at 0.5 bit/symbol** (a quarter
of the quick estimate, half the provisional figure, until the NIST run below says more): at every BT-mode entry the app
captures through the recorder's own path (gain 8, mono), discards the first 2048 samples (128 ms), feeds 4096 2-bit
symbols through the SP 800-90B RCT (cutoff 41) / APT (410 of 512) health tests into `bt_entropy` and mixes the full
frames with zero credit: first collection 1024 start-up + 3072 x 0.5 = 1536 credited bits (seed needs 384), later ones
2048 (reseed). About 0.4 s added to BT start. A health failure latches until reboot. If the recorder holds the ADC
(recording, VOR wait) nothing is captured and the session gets **no** new credit (no retry: see below); the ROM key
is then not replaced unless an earlier session of the same boot already seeded the DRBG, so SSP pairing stays
refused until the next BT-mode entry.

**Review fixes (2026-11-24, host only):**
- *Source sanity gate* (`bt_entropy_mic_frame_check`, host-tested in `tests/test_bt_entropy.c`): the RCT/APT at
  H = 0.5 catch only a source stuck on one symbol. Hard clipping (runs of `0x7FFF` = symbol 3 and `0x8000` = symbol 0,
  each run under the RCT cutoff, ~50 % in the APT window) passes them and would be credited in full (the test shows
  it). So each DMA frame's used samples are checked before any is credited: a sample with `|x| >= 32000` ends the
  collection (`... rejected: input at full scale ...`), and so does a frame with a peak-to-peak below 8 LSB (no
  analogue noise; Exp 2's near-uniform bit 2 at gain 8 implies a spread far above that). The log line now carries
  the collection's peak: `BT entropy mic: 4096 symbols (0:a 1:b 2:c 3:d) peak P in T ms, ok`. **Calibrate on
  silicon:** P in a quiet room at gain 8 (expected: tens to a few hundred LSB) tells how much headroom the 32000
  limit has; a quiet-room `rejected` means the limits are wrong, not the room.
- *No collection with the radio on.* The old retry every 5 s from the session poll sampled while the controller ran
  its 1600 Hz slot tick, a condition Exp 2 never measured, and blocked the poll for up to ~0.55 s each time. It is
  gone: the only collection is the one at BT-mode entry, before the controller starts.
- *REC switch:* the collection polls the REC switch and stops at once (`BT entropy mic: skipped (REC switch on), no
  credit this session`); key generation is skipped too (`BT P-192 key generation skipped: REC switch on`), so the
  BT-mode loop hands over to the recorder right away. Entry latency otherwise = mic T + keygen N + RF calibration:
  report T and N from step 7.
- *Power:* after the collection the AMIC1 PGA enables (`CODEC_AMIC01_PGA 0x20430000`) and `ANA_CTRL0`'s high
  enables (`0xC0000000`) are cleared again; the boot-time jack-detect bias (`ANA_CTRL0 0x22`, codec clock, global
  enable, `DAC_ANA_CFG0[9]`, keys.c) stays, and so does the APLL (as after a playback: its power-down is not
  traced). Unmeasured on silicon: compare idle current after a BT session with a fresh boot (step 9).

### Entropy source assessment (NIST SP 800-90B) — recipe for the lead
Raw data exactly as the firmware sees it: `open_fw/exp/exp_entropy_dump.c` **source 5** (sequential: 65,536 symbols per
run, packed 4 per byte) and **source 6** (restart rows: 16 x (capture start, discard 2048, 1000 symbols, stop) per run).
`arg = 5 | gain << 8` (gain 0 = the firmware's 8, unless `| 1 << 19`: gain field literal, so gain 0 can be
captured), `| 1 << 16` keeps the warm-up samples (start-up behaviour). **Control flags** (is the noise analogue?
Exp 2 listed this as still required and could not run it): `| 1 << 17` clears the AMIC1 PGA enables right after the
capture starts (unamplified, unbiased input), `| 1 << 18` additionally clears `ANA_CTRL0 0xC3000022` (analogue core
and MICBIAS; `0x22` is restored after the capture for the jack detect). The log line `control=` shows 0 / 1 / 2.
```bash
mkdir -p /tmp/ent                                          # any image with the exp runner; repo root as cwd
EXP="sudo env LLD=$(command -v ld.lld-20 || command -v ld.lld) OBJCOPY=$(command -v llvm-objcopy-20 || command -v llvm-objcopy) CLANG=$(command -v clang) python3 tools/dev_cycle.py exp"
for i in $(seq -w 1 16); do                                # 16 x 65536 = 1,048,576 symbols (>= 1M for ea_non_iid)
  $EXP open_fw/exp/exp_entropy_dump.c --arg 5 --dump-out /tmp/ent/seq_$i.bin
done                                                       # each log: symbols=65536 overruns=0 (discard a run with overruns)
for i in $(seq -w 1 63); do                                # 63 x 16 rows >= 1000 restart rows, one boot per run
  sudo python3 tools/dev_cycle.py dfu && sudo python3 tools/dev_cycle.py boot   # reset first: rows span 63 boots
  $EXP open_fw/exp/exp_entropy_dump.c --arg 6 --dump-out /tmp/ent/rst_$i.bin
done                                                       # (16 rows of one run share a boot; a codec dither LFSR
                                                           #  that survives capture stop/start would pass within one)
# Controls, gain 8: PGA off, then analogue path off; then gain 0 (literal) with the path on
$EXP open_fw/exp/exp_entropy_dump.c --arg $((5 | 1 << 17)) --dump-out /tmp/ent/ctl_pga_off.bin
$EXP open_fw/exp/exp_entropy_dump.c --arg $((5 | 1 << 18)) --dump-out /tmp/ent/ctl_ana_off.bin
$EXP open_fw/exp/exp_entropy_dump.c --arg $((5 | 1 << 19)) --dump-out /tmp/ent/ctl_gain0.bin
for c in pga_off ana_off gain0; do python3 tools/bt_entropy_unpack.py seq /tmp/ent/ctl_$c.sym /tmp/ent/ctl_$c.bin; done
python3 tools/bt_entropy_unpack.py seq /tmp/ent/mic_seq.bin /tmp/ent/seq_*.bin          # also prints a quick MCV
python3 tools/bt_entropy_unpack.py restart /tmp/ent/mic_restart.bin /tmp/ent/rst_*.bin
# NIST tool (C++; Ubuntu deps: sudo apt-get install libbz2-dev libdivsufsort-dev libjsoncpp-dev libssl-dev libmpfr-dev libgmp-dev)
git clone --depth 1 https://github.com/usnistgov/SP800-90B_EntropyAssessment.git /tmp/ea && make -C /tmp/ea/cpp non_iid restart
cd /tmp/ent && /tmp/ea/cpp/ea_non_iid -i -a -v mic_seq.bin 2         # -> H_I (min of H_original, 2 x H_bitstring)
/tmp/ea/cpp/ea_restart -n -v mic_restart.bin 2 <H_I>                   # -n = non-IID (-i would mean IID here)
```
(The tools want a relative file name. `ea_restart` must pass its sanity check; its validated value is the one to use.)
**Controls, expected:** with the analogue path off (`ctl_ana_off`) the low bits must collapse (quick MCV far below
the gain-8 run, or a constant / short cycle). If they stay near the gain-8 value, the "noise" is generated on the
digital side (codec dither or idle tones): do **not** credit the source until that is understood, even if
`ea_non_iid` passes (a deterministic generator can pass it). PGA off and gain 0 show how much of the noise comes from
the microphone path versus the ADC front end (both are electronic; report the MCVs).
Repeat on a second day / after a cold start, in a quiet room and with speech near the mic, and once at the user's
usual recording gain (`--arg $((5 | 20 << 8))`) to see how much the signal moves the low bits. **Decision rule:**
`BT_ENT_MIC_H_X16` (`bt_entropy_mic.h`, 1/16 bit per symbol) must stay <= half of the smallest assessed min-entropy,
and never above 16 (1 bit/symbol) without a written reason; the RCT/APT cutoffs in the same header are recomputed by
`tests/test_bt_entropy.c` (`make -C open_fw test-bt-port` fails if they no longer match the claim). Record the results
here.

### Mismatched public key (plan step 6b): result (2026-11-25, session 9)
Image: `make BT=1 BENCH=1 HP_GATES=1 KEYTEST_MISMATCH=1` from `5d45a5e` (FIRM CRC `0x2A66`): the session's private
key is staged with the public key of a second, independent `bt_p192_keygen` pair. Run `secs=150 pair=1 pair_secs=120
autoconfirm=1`; phone: Pixel 8 (Android), driven over adb (`uiautomator dump` + `input tap`: "Pair new device",
tap `Cassini_1D`, tap **Pair** in "Check pairing codes").

| Run | Image | Result |
|---|---|---|
| control | normal `029f2a8` (CRC `0x6B6A`) | phone code 458211 = device `value 458211`; `ev=0x36 st=0x00`, `BT SSP bonded (numeric comparison, MITM)`; phone stores the link key ~5 s after the tap |
| mismatch | `KEYTEST_MISMATCH=1` (CRC `0x2A66`) | `BT KEYTEST: staged public key belongs to another private key`, `installed 1 unexpected 0`; phone code 143671 = device value; **`ev=0x36 st=0x05`** (Authentication Failure, DHKey check), disconnect reason 0x16; phone `BOND_NONE hciReason:5` 2.5 s after the user approval, "incorrect PIN or passkey"; `BT SSP key spent` (window closed, device no longer discoverable) |

**Result: the ROM uses the staged pair** (public and private): with a fixed ROM pair the mismatch image would have
paired; with our public and the ROM's private the control would have failed.

Pitfall (cost two runs): Android's numeric-comparison dialog waits for **Pair** on the phone even when the device
side auto-accepts. Untapped, the phone gives up after ~36 s with the same "incorrect PIN or passkey" text, and the
device sees a late `ev=0x36 st=0x05` with disconnect reason **0x08** (timeout). The first mismatch attempt (user,
unclear whether Pair was tapped) and the first control (not tapped) are therefore not evidence; tell a real key
failure (reason 0x16, within seconds of the comparison) from a timeout (reason 0x08).

### SSP auto-accept (review C2 / A8, user decision): silicon check (2026-11-25, session 9)
Image `05f0057` bench build (FIRM CRC `0xB5DB`), run `secs=120 pair=1 pair_secs=90` (**no** `autoconfirm`), Pixel 8
over adb: `BT SSP numeric comparison accepted (pairing window open, auto-accept), value 240476` (= phone code),
`ev=0x36 st=0x00`, `BT SSP bonded (numeric comparison, MITM)`, phone stores the link key ~5 s after the tap. The
phone still shows its own "Check pairing codes / Pair" dialog (Android asks its user regardless).

### Entropy source assessment: results (2026-11-25, session 9)
Image: `DEV_EXP=1` default build from `integ/bt-all-local` `f078c9c` (FIRM CRC `0xA6BA`), gain 8, quiet room,
device on the desk, warm (after ~1 h of BT use). 16 sequential runs + 63 restart runs (one reboot each) + 3 controls:
81 / 81 captures with `overruns=0`. NIST `SP800-90B_EntropyAssessment` (GitHub HEAD, built against locally extracted
`libdivsufsort` / `libjsoncpp` / `libmpfr` packages).

| Data | Result |
|---|---|
| `mic_seq.bin` (1,048,576 symbols) | symbol counts 263050 / 261640 / 261249 / 262637; `ea_non_iid -i -a`: H_original **1.853**, H_bitstring 0.839 (compression estimate; next lowest 0.919 collision), **H_I = min(1.853, 2 x 0.839) = 1.677 bit / 2-bit symbol** |
| `mic_restart.bin` (first 1000 of 1008 rows: `ea_restart` wants exactly 1000 x 1000) | **Restart sanity check passed**; H_r 1.834, H_c 1.896; **validation passed, min(H_r, H_c, H_I) = 1.677** |
| control: PGA off (`1 << 17`) | H_original 0.206, H_bitstring 0.091, min 0.182 |
| control: analogue core + MICBIAS off (`1 << 18`) | **one symbol only** ("no entropy awarded"): the low bits collapse to a constant |
| control: gain 0 literal (`1 << 19`) | H_original 1.146, H_bitstring 0.483, min 0.967 |

Conclusion: the noise is analogue (it disappears with the analogue path off, so it is not a digital dither / idle
tone generator) and grows with the PGA gain. Assessed min-entropy 1.677 bit/symbol; the firmware credits
`BT_ENT_MIC_H_X16` = 8 (0.5 bit/symbol), below half the assessed value (0.84): **the claim stands, unchanged**.
Not yet done (lower priority, same recipe): a second day / cold start, speech near the mic, the user's recording gain.

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

## Session 3 (2026-11-24): Exp 1 stage B — PASS, discoverable from a laptop

Image: `BT=1 HCI_PROBE=1` at `977819a` (probe FIRM CRC-16 0x45B0), request `HCIPROBE.RUN` = `B` (consumed). Full log
from `SD:/HCIPROBE.LOG` (4,830 B, `lost=0`). Result: `END st=DONE fails=0 vfail=0` in 38.8 s; afterwards `legacy=allowed`.
Stage A repeated cleanly (Reset 205 ms, 5/5 restarts, BD_ADDR match).

| Item | Result |
|---|---|
| Five V-B writes, forwarded to the ROM (`fwd=1 romd=1`) | 0C56, 0C01, 0C24, 0C13, 0C1A all CC status 0 in **530–619 µs** (vs 150–330 µs for locally answered reads) |
| Read-backs | SSP host bit `01` (1004 p1); event mask `FF…FF` after Bluedroid's mask; name `Cassini-Probe-011D` via 0C14 (248 B match); zero-mask write reads back exactly the mandatory bits `00600400F80740E2`, then `FF…FF` again: **the ROM stores the mask we write, ORed with the mandatory bits** (and it is all-ones after Bluedroid's mask, since `BF 1D` sits on top of mandatory bits) |
| **Over-the-air** | Laptop (BlueZ, `bluetoothctl scan on`) saw `02:CA:55:00:01:1D` at RSSI −67…−75 dBm and resolved the name **`Cassini-Probe-011D`**: inquiry scan, inquiry response and the remote-name page/LMP exchange all run inside the ROM with no host involvement and no HCI events |
| IRQ 26 while discoverable (30 s) | 1607.6 Hz: bit 0 at 1600 Hz as at idle, plus **bit 1 at 7.5 Hz and bit 10 at 2.3 Hz** (scan-window activity; both serviced, 0 coalesced, `unh=0`). After `0C1A 00` only bit 0 remains |
| `bt_ctrl` CPU | idle 7.43 %, discoverable 7.40 %, scan off 7.29 % — scanning costs ~nothing on top of the 1600 Hz slot tick; per-100 ms peak 9.3 % (`cpu_pk` is in **permille**). Session 2 measured 5.3 % on the stage-A-only image; the slot tick, not radio activity, is the power cost to attack |
| Latency | IRQ→BH max 179 µs (avg 14), `bt_ctrl_run` max 287 µs while discoverable (plan bound 1 ms) |
| `g_bt_heap` | high-water **304 B** (the 248 B name message), 48 B in use during scan, 0 after teardown, `fail=0` |
| Free stack | bt_ctrl 2,936 of 4,096 B (1,160 used, same as QEMU), bt_dlv 1,372, probe 1,100 |
| RTOS heap free | 102,584 B |
| Unsolicited events | none (`evts=0`, `evt_unsol=0`) — inquiry scan produces no host events, as expected |

Also seen (not BT): `SDW_MDTO`/`SDW_MFAIL` once while writing `HCIPROBE.LOG` — the existing SD driver's multi-block
timeout falling back to single-sector writes; the log file was complete.

Next: the 22 G-B/G-C opcodes now have static mappings in `docs/BT_ROM_HCI_MAP.md` and need a stage that exercises them
(inquiry 0401/0402, remote name 0419, connection 0405, …); stage C (ACL) repeats the Exp 0 memory check.

## Session 4 (2026-11-24): phone mode on silicon — SSP + XREC PASS, then bt_ctrl fault 6 after a peer detach

Image: bench `BT=1 BENCH=1` at `a0792fa` (FIRM CRC-16 0x3599). Request `secs=300 pair=1 pair_every=60 pair_secs=60
autoconfirm=1 xrec_pin=246810 exit=auto`. Laptop: BlueZ `bluetoothctl` + `tools/bt_sync.py`; **the laptop was also
streaming A2DP to a speaker** on another ACL (handle 51) for the whole run. Recipe steps 1–5 of "Phone mode (BT=1) on
silicon" below; logs `SD:/BTBENCH.LOG` (3,262 B, `lost=0`), ring, laptop `btmon` trace.

| Item | Result |
|---|---|
| Entropy at BT entry | `4096 symbols (0:1040 1:983 2:1055 3:1018) peak 130 in 419 ms, ok` — symbol counts flat, peak well inside the clip/flatline limits (8 LSB … 32000) |
| Session P-192 key | `ms 67, public X 0xBACBE422...` (not the ROM constant `FA2B96D3`); `BT ROM keys: installed 1 unexpected 0 rng drbg 88 nonces 2 legacy 119 unknown 0 starved 0` — both pairing nonces came from the DRBG, the pool never ran dry |
| Discovery | laptop scan found `Cassini_1D` (cached name first, then the new one) |
| SSP numeric comparison | laptop `Confirm passkey 230749` → `Pairing successful`, `Paired: yes`, `Bonded: yes`, UUID `Serial Port`; device `bonded (numeric comparison, MITM)`, `window closed after the first bond`. **First SSP pairing ever on this ROM.** Proven in session 9: the ROM uses our key (mismatched-key image, step 6b) |
| XREC | `--ping` `PONG ... bound=0,auth=0,pair=1`; `--pair 246810` bound (`BT XREC PAIRED`); `--ping` `bound=1,pair=0`; `--list` CHAL/AUTH OK, 31 files; GETS of the 3,000 B test file **byte-exact** (SHA-256 match); `--stat` battery/SD line; wrong PIN inside window 2 → `ERR,PIN` |
| Reconnect | `bluetoothctl disconnect` while idle, reconnect 3 s later with the stored keys: AUTH OK, no re-pairing |
| Throughput | GETS 581,676 B in 25.7 s = **22.1 KB/s** steady (RFCOMM MTU 673, laptop radio shared with A2DP) |
| Memory | heap free min 61,968 B, largest block ≥ 33,536, `mallocfail 0`; lowest stack headroom hciT 431 words, bt_dlv 313 words |
| **Fault** | ~2 s after the big GETS the laptop dropped the idle ACL (HCI Disconnect 0x13 → Disconnect Complete at 21:34:25.9). The device never noticed: 20 s later `BENCH status t=120 acl=1 ... bound=1`, and from ~10 s after the detach it no longer answered pages (`Host is down`). When window 3 opened: `BT bt_ctrl fault, first code 6` (`BT_CTRL_FAULT_INVARIANT`), `platform fault 2`, graceful stop. **The recovery ladder worked**: no reboot, no fault record, the bench run finished on time and USB came back |
| Log bug | `numeric comparison accepted (BENCH autoconfirm), value 0` while the laptop showed 230749 |

The earlier idle disconnect at 21:33:52 was handled correctly; the failing one followed a long device→laptop ACL TX
burst. The fault-6 site was not logged (10 raise sites, no sub-code) — the fix round adds site ids. Exit-with-link-up
(step 6) could not be run because BT was already down. Bonds from this run are bench-only (laptop `remove`,
`SD:/bt_bonds.bin`).

**Diagnosis (host, `ac4a61e`):** the ROM's `0C1A` handler refuses page-scan values (2/3) with `0x0C` while it counts
more than one ACL link; the dead link plus the laptop's re-page made two, so the reopened window's `0C1A 03` came back
`0x0C` and `bt_ctrl` treated that class-M failure as INVARIANT (site 4). Now passed to the host and retried every 3 s
(fail-closed: scan off if a non-discoverable write is refused). Also patch 0028: the NC value was read before BTA set
it (logged/displayed 0 or a stale value).

## Session 5 (2026-11-24): phone mode re-run on `ac4a61e` — no fault, self-heals, exit with link up OK

Bench images `ac4a61e` (run 2, `secs=360 pair_every=60`) and `ac4a61e` + the `BT ROM links` flap filter (run 3,
`secs=200`). Laptop again streaming A2DP to a speaker.

| Item | Result |
|---|---|
| SSP NC | runs 2/3: laptop `445414` / `960773`, device logs the **same** value (patch 0028 confirmed) |
| Entropy / key | `(0:1031 1:1029 2:1015 3:1021) peak 152` / `(0:1042 1:1006 2:1046 3:1002) peak 137`, both `ok` in 419 ms; fresh public X each run (`BA2657F2…`, `84E270A6…`); `nonces 2 ... starved 0` |
| XREC | PAIR, AUTH, LS, GETS 581,676 B byte-identical across runs; 22.9 KB/s (run 2), 32.3 KB/s (run 3) |
| Stale link after an idle drop (run 2) | reproduced once: laptop Disconnect 22:38:28.9 → pages refused (`0x0F` / `Host is down` / timeout) until ~22:39:09, then every reconnect works. **No fault**; `fwd_to=1 late_reply=1` (one forwarded command answered late after the 7 s deadline), `scan_refused=0`. ~40 s self-heal |
| Detach handling (run 3) | 12 laptop-initiated idle drops, each logged `BT HCI disconnection complete h=0x0080 st=0x00 reason=0x13` and followed by a clean `connection request/complete`; `fwd_to=0`, no fault. The stale link did not reproduce (2 of 3 runs so far, one occurrence each) |
| Exit with a link up (run 2) | RFCOMM socket held open over `secs`: `BENCH exit` → `BENCH end` in **0.74 s**, laptop saw `Connection reset by peer`, no reboot, no fault record |
| Memory | heap free min 61,608 B, `mallocfail 0`; lowest stack headroom bt_dlv 313 words, hciT 425 words |
| ROM link bytes | LC state per slot: `00` free ↔ `03` scanning (flaps at the scan rate — filtered for logging after run 2 flooded the 8 KiB bench log, `lost=60858`), `0A`/`62`/`29`/`3A`/`82` connection setup, `6A` pairing, `05` connected, `26` during L2CAP/RFCOMM teardown; LD `01` connected, `FF` free; `rep` `01` while a link exists |

## Session 6 (2026-11-24): confirm prompt with a person at the device — PASS

Release `BT=1` image at `488da72` (FIRM CRC-16 0x4E20), entered through the Bluetooth tile, window opened with ENTER;
laptop BlueZ `agent DisplayYesNo`. A person pressed the keys and read the screen.

| Test | Result |
|---|---|
| Window | first ENTER... screen `Cassini_1D` but no PIN and the laptop could not find the device (the tile only shows the name; the window needs ENTER) → ENTER: XREC PIN `213197` shown, device discoverable |
| Accept (ENTER) | device prompt `333198` = laptop passkey; peer shown as **address, no name**; ENTER → laptop `Paired: yes, Bonded: yes`; XREC `--pair 213197` bound; screen `verbunden` |
| Reject (BACK) | prompt `194890` = laptop; BACK → laptop `AuthenticationFailed`, device **stays in BT mode** |
| Timeout | prompt `987079`; `Pair?` countdown to 0 → laptop `AuthenticationFailed`; screen back to the window with the PIN, which disappeared when the 60 s window ended |
| REC slide in BT mode | leaves BT mode and records (`REC_0028.cwx`, 83 KB for ~5 s) |
| BACK without a prompt | leaves BT mode (expected; the operator pressed it before the laptop had started pairing) |

Findings (to fix): (1) the prompt showed the laptop's address, not its name: the first-pairing remote-name request
evidently fails or comes too late on silicon (patch 0028 already copes with the failure; check the `0419` path / ROM
name request). (2) After the rejected pairing the status line still said `verbunden` although the laptop had
disconnected. The ring log was lost to a power cycle (the operator rebooted to get USB), so neither has device logs yet.

Open: why the ROM sometimes keeps a dead link for ~40 s after a remote detach (H1/H2 in `docs/BT_ROM_HCI_MAP.md` §11).
The next occurrence on a filtered-log bench image will show the LC/LD bytes and which command hit `fwd_to`.
Not yet on silicon: the on-device confirm prompt (needs a person: ENTER/BACK), the mismatched-public-key image
(step 6b), Just Works refusal, the peer-gone exit, NIST entropy runs.

## Session 7 (2026-11-25): headphones mode, first silicon runs, laptop as sink

Bench images `BT=1 BENCH=1 HP_GATES=1` from the headphones branch (commit `9a6dd5a` holds the fixes); the laptop
(BlueZ, MediaTek controller, PipeWire) played the headset. Logs: `SD:/BTBENCH.LOG` per stage (8 KiB head: the longer
runs lost their tails, `LOGCAP lost=2252..2451`), laptop `btmon`.

| Stage | Result |
|---|---|
| `scan` (`hp_secs=10`) | `inquiry st=0x00 rom`, `inquiry complete results=13`; the laptop found (`cod=0x6C010C audio=1`), name pass `martianbfg` (`name complete st=0x00 len=10`); teardown 217 ms |
| `connect` (`hp_addr`, `hp_pair=1`) | page as master: `create connection st=0x00 rom`, link up at **150 ms**; name OK; then **legacy PIN** instead of SSP: `BT hp legacy PIN pairing rejected`, `auth st=9`, `connect failed`, clean `0406`. Bench HCI trace: `0411` CS → `ev=0x17` → `040C` CC → `ev=0x16` (PIN Code Request), two `041C` reads before it. Cause: the ROM's single "remote page 1" slot overwritten by the page-2 read (`docs/BT_ROM_HCI_MAP.md` §12) → **fixed by patch 0040** |
| `tone` with `hp_pin=1` (before 0040) | legacy pairing with `0000` (`ev=0x18`, `auth st=0 key=0`), then the full path below: the rest of the stack was fine |
| `tone` with 0040 | one `041C` (page 1), `ev=0x31/0x32/0x33`, `BT hp Just Works pairing accepted`, `auth st=0 key=4`; codec `cie=0x23021511` 48 kHz joint stereo 16 blocks 8 subbands bp 2-35, `mtu=666`, `role=M max_slots=5 edr=3`; delay reports 30.6 / 70.6 ms |
| streaming (tone, 16 kHz source) | 48 kHz, `bp=35 bp_eff=24` (speech bitpool), `fpp=8`: ~375 SBC frames/s (`sbc=352..432`), `pkts` ~49/s, `Bps` ~23 kB/s (21.1-23.9 k), `acked` = `sbc` ±8, **lost 0 drop 0 pads 0 allocf 0 stalls 0**, `heap_min` 44016; AVRCP CT/TG up, `rn caps bits=0x2D1E abs_vol=1` |
| `connect` with 0040 + 0041 | SSP Just Works as above; BlueZ kept the bond (without 0041 our IO capability reply asked for no bonding and BlueZ dropped the key at disconnect); teardown with the link up **647-658 ms** |
| `player` | the run hung past `secs` inside a 120 min recording: the player blocked the bench's time limit → **bench fix** (the player's UI tick drives the bench and injects BACK at the limit, `BENCH hp play: time limit, BACK`) |

Not yet captured: a laptop recording of the stream. PipeWire showed the bluez card in the `audio-gateway` profile with
no A2DP source node, and the transport stayed idle in the player stage; unexplained (next run: `pactl list cards`,
`wpctl status`, `btmon` around the AVDTP START, and whether the player stage's media START was sent at all).

Seen in the trace, expected: `080D`, `0C37` (CC `0x01`) and `040F` (CS `0x01`) are refused locally by `bt_ctrl`
(REJ rows); `ev=0x1B` Max Slots Change right after the Connection Complete. The first trace images also traced
successful set-up CCs (`0C03`...`0C14`); the committed trace skips successful CCs outside link control.

## Session 8 (2026-11-25): headphones audio captured on the laptop

Image: bench `BT=1 BENCH=1 HP_GATES=1` from `integ/bt-all-local` `64d820e` (FIRM CRC `0xD52F`); laptop as sink
(PipeWire 1.6.2, BlueZ 5.85). Capture: PipeWire creates `bluez_input.<addr>.N` only while the transport is
`active` (after AVDTP START); `pw-record --target <node> -P '{ node.dont-reconnect = true }'` records it (without
`dont-reconnect`, pw-record moves to the default source when the node goes away and records laptop audio).
WirePlumber auto-links the node to the default sink; `pw-link -d` on those links keeps it off the speakers.

| Run | Result |
|---|---|
| first boot of the new image (`player`, `hp_pick=last`) | **bond lost**: the stored key was not used (no `0411` before the SSP attempt, `ev=0x36 st=0x05`), `Just Works pairing refused` (no window: correct), retries 3/10/30 s; `bt_hp_bonds.bin` went from 152 to 43 B (empty store written). Previous boot ran the older image `0edcb61`. Cause not found (ring tail only); same-image reboots afterwards kept the bond (next rows). Analysis and the store hold fix: `docs/reviews/STEP10_JOINT_REVIEW.md` HW8 (an empty start over a non-empty file can no longer replace it; a peer that lost the key still removes the bond) |
| `connect hp_addr hp_pair=1` (laptop bond removed first) | SSP Just Works accepted inside the window, `auth st=0 key=4`, `bt_hp_bonds.bin` 152 B |
| `player hp_pick=last secs=70` | reconnect **with the stored key** (no pairing), link 171 ms, `connected` 3.4 s, plays `/cassini/cassini-final-moments.wav` (IMA ADPCM 16 kHz mono); BlueZ `fd(30) ready`; the run ends at `secs` (time-limit BACK works). Capture: audio present but **~42 dB below the source** and **`pads=5..17` per second** (packets with no real PCM) |
| `tone hp_pick=last secs=40` | capture: continuous **1000 Hz** (zero-crossing count 992-1000 Hz), constant RMS for 31 s, no gaps; device `pkts` 47-55/s, `acked` 368-440, pads 0 |

Findings:
- **Level**: AVRCP `abs_vol=1` arrived ~1.3 s after the player set the PCM gain, so the gain stayed at `bt_vol` 64
  (vol31 16, -24 dB) while the sink applied absolute volume 64 as well. Fixed in `ecc8b33` (the player re-applies
  the gain when `bt_hp_abs_vol()` changes).
- **Player starvation**: the tone path (same ring, resampler, SBC) has pads 0; the player stage has ~20 % padding
  packets, i.e. audible gaps. Under investigation on `fix/hp-player-starve`.

## Session 9 (2026-11-25): user headset (Jabra Hear Me Out) after the rate/quality fixes

Image: bench `BT=1 BENCH=1 HP_GATES=1` from `integ/bt-all-local` `d6ca189` (FIRM version `0x0B03`). Fixes under
test: mono source writes on the headset route, `.bt_fast` SRAM section, `HP_CREDIT_TICKS` 4, SMLADX resampler,
`bt_ctrl` in SRAM, advertised max bitpool 53 (was 35), names/toasts UX (`d6ca189`).

Run: `mode=hp hp_stage=player hp_addr=08:C8:C2:82:6C:EB hp_pair=1 secs=60`, headset in pairing mode, no stored key.

| Item | Result |
|---|---|
| pairing | SSP Just Works, `auth st=0 key=4`, connected 4.3 s after start |
| name | `name saved ... name=Jabra Hear Me Out` (remote name request after auth) |
| codec | 48 kHz, `bp=53 bp_eff=53`, 7 frames/packet |
| rate | `sbc` 378-399 frames/s per 1 s window (real time = 375; before the fixes 362.8 = 96.7 %) |
| gaps | `pads=2` in the first second, then `pads=0`; `lost=0 drop=0` |
| CPU | idle 6-16 %; BTC_TASK (SBC) 28-34 %, app (player) 22-32 %, bt_ctrl 13-17 %, BTU 8-12 %, hciT 4-7 % |
| bt_ctrl split (per s) | ROM ISR ~48 ms, ROM kernel 5-8 ms, glue 44-56 ms over ~1750 runs |
| listening | user: "It worked nicely. I think. I can hear it" |

Notes:
- `BTBENCH.LOG` (8 KiB) filled after ~11 s of play (`LOGCAP lost=8416`); the per-second `cpu`/`tasks`/`ctrl`
  lines use ~600 B/s. Use `secs=15` for measurement runs; longer runs only for listening.

Normal UI on the same image (no RUN file), user report "All worked flawlessly":

| Check | Result |
|---|---|
| Settings → Bluetooth Forget row | shows "Forget Jabra Hear Me Out" (name, not MAC) |
| output `BT`, headset on | plays in the headset; clean, no periodic hiccup |
| output `Both`, headset on | plays on the speaker and the headset together |
| output `BT`, headset off, PLAY | shows the not-connected toast instead of a silent stall |

Review C5 / A11 run: image `029f2a8` (bench lines `hp ctrl ... more streak_max streak_us`, `hp stack` = FreeRTOS
`usStackHighWaterMark`, free words, lowest since task creation), `player hp_addr=<Jabra> secs=20`, stored key
(no pairing): connected 1.3 s after start, 371-420 SBC frames/s, `pads=0`, idle 9-18 %.

| Item | Result |
|---|---|
| C5 (`bt_ctrl` zero-wait loop) | `more=0` in every window and **`streak_max=0 streak_us=0`** since boot (ROM start, page, auth, AVDTP setup, streaming): `bt_ctrl_run()` never returned "more work" on silicon, ~1700-1900 runs/s at 49-58 us each. The zero-wait path is not exercised; no yield bound needed. |
| A11 (stack headroom, words free) | BTC_TASK **543** (2172 B), BTU_TASK 869, bt_ctrl 721, hciT 425, bt_dlv 313, bt_recover 695, bt_alarm 191, Tmr Svc 356, IDLE 103, app 2065. All far above the 512 B threshold. Caveat: SSP pairing was not in this run (stored key); the session-9 pairing run predates the `hp stack` line. |

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
the image logs `HCIP OFF trig=absent` and runs without Bluetooth (diagnostic image: no phone or headphones mode). A probe that hangs or resets the device
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
assume the device stopped being discoverable. RW-BT stays claimed until that reboot anyway.
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
  sets one), and reports whether RW-BT may be in an unknown state: from the first RF calibration until a `stop()` whose
  soft reset completed, and after `HANG` / `HARDSTOP`. `HCIP RELEASE` logs both (`ipr`, `orig`, `claimed`, `lockout`);
  reboot before using Bluetooth after `claimed=1` or `lockout=1`. Since plan step 9 the probe image has no phone or
  headphones mode at all (the Bluetooth tile shows a toast); logs from before carry `legacy=allowed|refused` too
  (`tools/bt_probe_report.py` reads both).
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
what=irq26` → teardown → retry → `RELEASE ... claimed=0 owner=0 lockout=0` → `END st=ABORT` → `LOGFILE ... st=OK`, with the normal
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
  **Fixed (2026-11-24):** a reported `test-qemu-hciprobe` failure "with `HCIP TIMEOUT what=irq26` after a 514 ms
  Reset" was this same step 3 (`[FAIL] btport (detail=3)` / "RTOS selftest reported failures"; the TIMEOUT line is the
  *expected* QEMU outcome and only happened to be printed next to it): reproduced with the first Reset at `lat=534897`.
  Step 3 now measures the elapsed time with `esp_timer_get_time()` and judges "not early" only when less than 5 ms
  passed (`src/rtos/rtos_selftest.c`, QEMU self-test images only). The QEMU-only probe images (`HCI_PROBE_AUTORUN` /
  `_WAIVE_LIVENESS`) also get 5x the Reset / hang / teardown deadlines (`HCI_PROBE_QEMU_SLACK` in `hci_probe.c`,
  `BEGIN ... dl_reset=10000000 hang=15000000`), so a slow host cannot turn the expected liveness TIMEOUT into a Reset
  timeout, HANG or HARDSTOP; the silicon probe image is unchanged (identical object code, 2 s / 3 s / 1 s).

## Phone mode (BT=1) on silicon: how to test

One ordered recipe for the lead: steps 1-8 need **no key press on the device** (the `SD:/BTBENCH.RUN` bench trigger
of a **bench image**, `make BT=1 BENCH=1`, enters BT mode, opens the pairing windows, confirms the numeric comparison
and supplies a known XREC PIN), step 9 lists the few checks that need a person at the device, step 10 is the entropy
assessment. Nothing of phone mode has
run on silicon yet; the host runs the same path end to end (`make -C open_fw test-bt-host`, `tests/bt_host/README.md`
scenario P, steps P1-P20, against the fake ROM).

### What the image does (2026-11-24)

- **Session:** every `BT=1` image without `HCI_PROBE` replaces the legacy phone mode with the Bluedroid session
  (`open_fw/bt_port/bt_xrec_bluedroid.c` + `bt_xrec_fw.c`, plan M4). Name `Cassini_1D` (eFuse address
  `02:CA:55:00:01:1D` on the bench unit), CoD 0x200404, SPP "Cassini" on RFCOMM channel 1, connectable; discoverable
  only while the SSP part of a pairing window is open. The ROM's answers to the newly forwarded commands
  (`docs/BT_ROM_HCI_MAP.md` §10) are the first thing this test checks.
- **Entropy** (deviation 5 closed pending this test): at every BT-mode entry `bt_entropy_mic.c` captures 4096 2-bit
  mic symbols (Exp 2 section above, 0.5 bit/symbol credit) and seeds/reseeds the DRBG (~0.4 s). The XREC PIN,
  `+XREC:CHAL` nonces and everything below come from the DRBG; without credit all of them fail closed (no PIN:
  `+XREC:PAIR` answers `ERR,PAIRCLOSED`; `CHAL` closes the link).
- **SSP key material** (deviation 8 closed pending this test; `docs/BT_ROM_INTERFACE.md` §2.3): the ROM's own P-192
  key pair is a constant, the same on every SL6801. Each BT-mode entry generates a fresh pair from the DRBG and
  `bt_rom_real` installs it at HCI Reset (only if the RAM slots hold exactly the ROM constant); the RNG hook gives the
  nonce, LMP-random and passkey call sites DRBG bytes (full 128-bit Na/Nb). Until step 4 has passed on silicon, treat
  bonds as bench-only (delete them afterwards: `SD:/bt_bonds.bin`, the laptop's `remove`).
- **Pairing gate (M2.3):** the SSP part of a window (discoverable + numeric comparison) opens only when entropy is
  credited, this session installed the key and the hook's pool never ran dry; re-checked on every poll, and again
  when a comparison arrives and when the user (or the bench autoconfirm) accepts it, so a pairing whose nonce met a
  dry pool between two polls is rejected (`BT SSP numeric comparison rejected, pairing window closed: <reason>`).
  **Spent key:** an accepted comparison whose pairing then fails (or whose link drops first) closes SSP for the rest
  of the BT session (`BT SSP key spent: ...`; the ROM never validates the peer's key, so each failed accepted pairing
  is one invalid-curve query against this session's key: `docs/BT_ROM_INTERFACE.md` §2.3 item 4); leave and re-enter
  BT mode for a new key. A window
  opened before the session is READY (the bench run opens it right after `bt_xrec_start()`; the controller, and with
  it the key install, starts later) is decided at READY (`BT SSP pairing window open (opened before READY)`), not
  refused early. Refused: `BT SSP pairing window refused: <reason>` and the screen toast "Pairing unavailable"; without
  any entropy nothing opens.
- **Prompt** (deviation 1 resolved): a numeric comparison inside the window shows the six digits, the peer (remote
  name, else address), `Pair? N s` and `ENTER Yes · BACK No`; ENTER accepts, BACK rejects (stays in BT mode), 30 s
  without an answer rejects, keys in the first 0.5 s are ignored, the window closing or the peer giving up ends it.
  ENTER / BACK within 1 s **after** a prompt ended (answered, timed out, withdrawn) are swallowed, so a late "No"
  does not leave BT mode and a double ENTER does not re-open the window. ENTER while a window still has more than
  10 s left extends it and **keeps** its XREC PIN (`BT XREC pairing window extended, PIN kept`).
  The SSP part closes at the **first** MITM bond; the XREC PIN stays for the rest of the window.
- **Bench overrides** (bench images only: `make BT=1 BENCH=1`; `SD:/BTBENCH.RUN` only, both off by default, switched
  off again when the run ends, logged on every use; never set by the UI). A **release** `BT=1` image contains neither
  the trigger nor the overrides (review finding: card or USB-disk access alone could arm an unattended pairing;
  `make test-bt-xrec` checks the release session object for bench symbols): `autoconfirm=1` accepts numeric comparisons inside the window without the user
  (`BT SSP BENCH autoconfirm: ...`), `xrec_pin=NNNNNN` makes every window of the run issue that XREC PIN instead of a
  DRBG draw (`BT XREC BENCH PIN: ...`; still only with credited entropy). Neither bypasses the gate: with the gate
  refusing, a comparison is rejected even with `autoconfirm=1` (host step P18).

### The bench trigger `SD:/BTBENCH.RUN`

The laptop side can be driven remotely, the device's buttons cannot, and USB is not serviced while BT mode runs.
A **bench image** (`make BT=1 BENCH=1`; not with `HCI_PROBE=1`) therefore takes a **one-shot request file** in the SD
root (release `BT=1` images ignore it: no `BENCH` code and no `BENCH` log line at all)
(`open_fw/bt_port/bt_bench.h`, `bt_bench.c`, `bt_bench_fw.c`; hooks in `src/main.c`): after the SD mount and the first
UI render, before the home loop, the boot reads `BTBENCH.RUN`, **deletes it and verifies the deletion before BT
starts** (a hang, WDT cold reset, BT-fault reboot or `reboot=1` never runs it twice; an undeletable request does not
run: `BENCH OFF trig=stuck`), enters BT mode exactly as the Bluetooth tile does (`run_bt_xrec_mode()`), opens the
pairing window on schedule, and after `secs` leaves BT mode through the normal exit (`bt_xrec_stop()`, as BACK). Then
the boot continues normally: USB disk mode comes up by itself (VBUS) and the log can be read. BACK / REC slide on the
device still end the run early (`BENCH end why=user`).

Content: `key=value` tokens separated by spaces, newlines, `,` or `;`; `#` comments; case-insensitive; BOM/CRLF ok.
Empty file = all defaults. Unknown or repeated keys and out-of-range values are **refused** (the file is still
consumed): `BENCH OFF trig=bad_content err=key|value|dup|conflict|char at=<byte offset>`.

| key | values (default) | effect |
|---|---|---|
| `secs` | 10..3600 (180) | time in BT mode, counted from `bt_xrec_start()` |
| `pair` | 0/1 (0) | open the pairing window at once (`bt_xrec_open_pairing`; its SSP part opens at READY, ~2-5 s later) |
| `pair_every` | 0 or 10..3600 (0) | re-open the window every N s (needs `pair=1`) |
| `pair_secs` | 10..600 (60) | length of each window |
| `autoconfirm` | 0/1 (0) | bench override: numeric comparisons inside the window accepted without ENTER. With 0 the comparison waits for ENTER on the device and is rejected after 30 s |
| `xrec_pin` | 6 digits (none) | bench override: the XREC PIN of every window of the run (needs `pair=1`); none = a fresh DRBG PIN per window, shown only on the screen |
| `exit` | `auto` | the only mode (leave after `secs`); accepted for readability |
| `reboot` | 0/1 (0) | after BT mode and `BTBENCH.LOG`: `sd_flush` + reboot |

Log (ring and file): `BENCH start secs=.. pair=.. pair_every=.. pair_secs=.. autoconfirm=.. xrec_pin=0|1 exit=auto
reboot=.. prev_fault=0x........ log=BTBENCH.LOG` · `BENCH pair n=N t=S secs=W` per window · `BENCH status t=S acl=
spp= bound= auth= pair= rx= tx= pairs=` every 30 s and at the end · `BENCH exit why=time ms=..` · (`BT stopped` etc.)
· `BENCH end why=time|user ms=.. pairs=.. reboot=..` · ring only: `BENCH LOGFILE file=BTBENCH.LOG st=OK|FAIL bytes=
lost=`. Without a request every bench-image boot logs `BENCH OFF trig=absent file=BTBENCH.RUN`. An image without the
session's overrides logs `BENCH autoconfirm st=unavailable` / `BENCH xrec_pin st=unavailable` (weak no-op defaults).
`SD:/BTBENCH.LOG` (replaced each run) holds every ring line from `BENCH start` to `BENCH end` (drained from the 4 KiB
ring on every BT-loop pass into an 8 KiB SRAM_HI buffer), followed by `BENCH LOGCAP bytes= lost= resets=`: `lost` > 0
means the buffer filled (the file keeps the head, the ring keeps the tail); `resets` counts XREC log resets. A run that
crashes or takes the BT hard path loses the file (SRAM, zeroed at boot): read the ring and `dev_cycle.py status`.
RAM: the buffer, writer and state live in `.bss_hi` (8.9 KiB; `.bt_bss` would come out of the RTOS heap): the BT=1
phone image ends `.bss_hi` 7.4 KiB below the `0x00867000` main-stack limit (`sl6801_firm.ld` ASSERT).

### Recipe

Shell variables used throughout (repo root as cwd; bench unit address):
```bash
MAC=02:CA:55:00:01:1D
PIN=246810                                          # the run's XREC PIN (any 6 digits)
B=/tmp/sl6801_openfw_bt_bench
```

**1. Build and flash the bench image** (`BENCH=1`: the trigger below; `update-up` writes `dumps/firmware_2M_openfw.bin`,
which the QEMU gates overwrite: build right before flashing):
```bash
make -C open_fw BT=1 BENCH=1 BUILD_DIR=$B update-up   # 0 warnings; "Packed hybrid FIRM ..."
sudo env OPENFW_FIRM_BIN=$B/open_fw_firm.bin python3 tools/dev_cycle.py flash
```
The boot after flashing has no request: its ring log ends with `BENCH OFF trig=absent` and USB disk mode comes up.

**2. Prepare the card** (device in USB disk mode: home screen + cable). A small known file under `record/` is the GETS
test object; `bt_bonds.bin` / `cassini.key` are removed for a clean first pairing:
```bash
lsblk -o NAME,MODEL,FSTYPE,SIZE,MOUNTPOINT          # the exFAT partition, e.g. /dev/sda1
udisksctl mount -b /dev/sda1                        # "Mounted /dev/sda1 at /media/$USER/<label>"
M=/media/$USER/<label>
rm -f $M/BTBENCH.LOG $M/bt_bonds.bin $M/cassini.key
mkdir -p $M/record && head -c 3000 /dev/urandom > $M/record/BTTEST.WAV && sha256sum $M/record/BTTEST.WAV
printf 'secs=300 pair=1 pair_every=60 pair_secs=60 autoconfirm=1 xrec_pin=%s exit=auto\n' $PIN > $M/BTBENCH.RUN
sync && udisksctl unmount -b /dev/sda1
```

**3. Start the run.** USB stays away for `secs` (BT mode), so wait for the boot in the background:
```bash
bluetoothctl remove $MAC                            # the laptop forgets any old bond (the device's store is empty)
sudo btmon -w /tmp/bench.btsnoop > /dev/null 2>&1 &
sudo python3 tools/dev_cycle.py dfu
sudo python3 tools/dev_cycle.py boot --timeout 400 > /tmp/bench_boot.txt 2>&1 & BOOT=$!
sleep 15                                            # BT mode + entropy (~0.4 s) + bring-up; window 1 runs t=0..60 s
```

**4. SSP pairing (laptop BlueZ; the device confirms by itself).** Scripted `bluetoothctl` (untested as a pipe on this
bench; if it misbehaves, type the same commands interactively and answer `yes` to the passkey question):
```bash
{ echo 'agent DisplayYesNo'; echo 'default-agent'; echo 'scan on'; sleep 8; echo "pair $MAC"; sleep 4; echo 'yes';
  sleep 6; echo "trust $MAC"; echo "info $MAC"; echo 'scan off'; echo 'quit'; } | bluetoothctl | tee /tmp/bench_pair.txt
grep -E 'Confirm passkey|Paired:|Bonded:|Serial Port' /tmp/bench_pair.txt
```
Expected: `Cassini_1D` found by the scan, `[agent] Confirm passkey NNNNNN`, `Pairing successful`, `Paired: yes`,
`Bonded: yes`, UUID `Serial Port (00001101-...)`. NNNNNN must equal the value in the device log line `BT SSP numeric
comparison accepted (BENCH autoconfirm), value NNNNNN` (step 7). A refused pairing (`AuthenticationFailed`,
`Failed to pair`) with `BT SSP pairing window refused: <reason>` in the log is the M2.3 gate: report the reason.

**5. XREC, in this order (all expected to pass now; window 1 is still open, or wait for the next one at 60/120 s):**
```bash
python3 tools/bt_sync.py --mac $MAC --ping          # (a)
python3 tools/bt_sync.py --mac $MAC --pair $PIN     # (b) PAIR with the run's XREC PIN
python3 tools/bt_sync.py --mac $MAC --ping          # (c)
python3 tools/bt_sync.py --mac $MAC --list          # (d) CHAL/AUTH + LS
python3 - "$MAC" <<'PY'                             # (e) GETS of the small file, byte-exact
import hashlib, sys; sys.path.insert(0, "tools")
from pathlib import Path
from bt_sync import XRecRFCOMMClient, load_device_key, DEFAULT_KEY_FILE
mac = sys.argv[1]
c = XRecRFCOMMClient(mac)
c.connect_and_handshake(); print(c.ping())
c.authenticate(load_device_key(DEFAULT_KEY_FILE, mac)); print("AUTH ok")
files = dict(c.list_files()); print(files)
print("sha256", c.download_file("BTTEST.WAV", files["BTTEST.WAV"], Path("/tmp/BTTEST.WAV")))
c.close()
PY
python3 tools/bt_sync.py --mac $MAC --stat          # (f)
python3 tools/bt_sync.py --mac $MAC --pair 000000   # (g) negative: window closed by (b)
```
Expected: (a) `Native SPP (OpenFW): device silent after connect, skipping the HFP handshake.` and `Ping response:
+XREC:PONG,openfw_v5,v5,bound=0,auth=0,pair=1` (`pair=1`: the window's PIN exists, i.e. entropy was credited; `pair=0`
here means no PIN: check the entropy lines of step 7). (b) `Paired & bound ... successfully!`; the PAIR closes the
window and discoverability (`BT XREC PAIRED`, `/cassini.key` written). (c) `bound=1,auth=0,pair=0`. (d)
`Authenticated (HMAC-SHA256 challenge-response OK).` and a listing that contains `BTTEST.WAV (3,000 bytes)`. (e) the
same SHA-256 as step 2. (f) battery / SD line. (g) `ERR,PAIRCLOSED` until the next window opens (at 60 s / 120 s it
opens again with the same PIN; inside a window a wrong PIN gives `ERR,PIN`, five of them close the window).
Then *reconnect with the stored keys*: `bluetoothctl disconnect $MAC; sleep 3; python3 tools/bt_sync.py --mac $MAC
--list`: no pairing, CHAL/AUTH again.

**6. Optional checks in the same run** (no key press; while `secs` lasts):
- *Exit with a link up:* about 20 s before the run ends, keep a raw socket open
  (`python3 -c 'import socket,time;s=socket.socket(socket.AF_BLUETOOTH,socket.SOCK_STREAM,socket.BTPROTO_RFCOMM);s.connect(("'$MAC'",1));time.sleep(60)'`):
  at `secs` the device must leave BT mode in about a second with no reboot (host: ~0.64 s, patch 0026) and the laptop
  sees the link drop.
- *Exit while the peer is gone* (instead of the previous one, in a separate run): with the socket open, cut the laptop
  radio without a clean disconnect (`sudo btmgmt power off`) a few seconds before `secs`; the device must leave BT mode
  in under a second, **no reboot, no fault record** (before patch 0027 this crashed in `esp_bluedroid_deinit()`).
  `sudo btmgmt power on` afterwards.
- *One bond per window:* from a second laptop/phone inside the same window after step 4: not discoverable any more,
  numeric comparison refused (`BT SSP window closed after the first bond`, `... rejected: pairing window closed`).
- *Just Works refused:* a second run with `bluetoothctl` `agent NoInputNoOutput`: BlueZ reports success briefly, then
  the device removes the bond and drops the link (`BT SSP bond removed: Just Works / unauthenticated key`); `--ping`
  from that laptop gets **no** `+XREC:PONG`; `SD:/bt_bonds.bin` never contains its address.
- *Prompt timeout:* a run with `autoconfirm=0`: `pair` from the laptop; nobody answers on the device: `Failed to pair`
  after 30 s, `BT SSP numeric comparison rejected: no answer within 30 s` (or `BT SSP prompt withdrawn: pairing ended
  by the peer` if BlueZ gives up first). Note whether the laptop's name or its address showed on the prompt.

**7. Read the results** (after `secs` the device leaves BT mode and enumerates over USB):
```bash
wait $BOOT; cat /tmp/bench_boot.txt                 # dev_cycle snapshot (previous fault, if any)
sudo python3 tools/dev_cycle.py log > /tmp/bench_ring.log            # ring first, before anything else logs
udisksctl mount -b /dev/sda1 && cp $M/BTBENCH.LOG $M/bt_bonds.bin $M/cassini.key /tmp/ ; udisksctl unmount -b /dev/sda1
grep -E '^BENCH|^BT ' /tmp/BTBENCH.LOG
sudo pkill btmon
```
Must be present, in this order (`BTBENCH.LOG`):
- `BENCH start secs=300 pair=1 pair_every=60 pair_secs=60 autoconfirm=1 xrec_pin=1 exit=auto reboot=0 ...`,
  `BT start (Bluedroid):` `Cassini_1D`.
- **Entropy and ROM key** (inside `bt_xrec_start()`, before the bring-up): `BT entropy mic: 4096 symbols (0:a 1:b 2:c
  3:d) peak P in T ms, ok` (a..d each roughly 1000; a count near 0 or 4096 means stuck bits; T about 300-500; **report
  P**: the quiet-room peak at gain 8 calibrates the sanity gate, whose limit is 32000; `rejected: input at full
  scale` / `rejected: no analogue noise` in a quiet room means the gate's limits are wrong) and `BT P-192 session key
  generated (replaces the ROM's fixed key), ms N, public X hhhhhhhh...` (report N; X must not be the ROM constant's
  `FA2B96D3` and must change between runs). T + N is the extra delay before a REC slide right after entering BT mode
  takes effect (the collection itself stops at once on REC).
- `BT SSP BENCH autoconfirm ON ...`, `BT XREC BENCH PIN ON ...`, `BT XREC BENCH PIN: fixed PIN ...`, `BENCH pair n=1
  t=0 secs=60`, then `BT XREC READY (SPP channel 1)` and `BT SSP pairing window open (opened before READY)`.
- `BT SSP BENCH autoconfirm: numeric comparison accepted WITHOUT the user (bench only)`, `BT SSP numeric comparison
  accepted (BENCH autoconfirm), value NNNNNN` (= the laptop's passkey), `BT SSP bonded (numeric comparison, MITM)`,
  `BT SSP window closed after the first bond`, `BT XREC SPP open, mtu N`, `BT XREC PAIRED`.
- `BT mem run heap free F min M largest L mallocfail 0` and `BT stack free words ...` every 30 s (`min` >= 8192,
  `mallocfail` 0; a stack value near 0 is about to overflow), `BENCH status ...` every 30 s, `BENCH pair n=2 t=60`,
  `n=3 t=120` ...
- `BENCH exit why=time`, `BT XREC SPP closed` (if a link was up), `BT mem stop ...`, **`BT ROM keys: installed 1
  unexpected 0 rng drbg D nonces K legacy L unknown 0 starved 0`** (I counts the key installs since boot: 1 in a
  fresh boot; **unexpected 0**, else the ROM's RAM slots did not hold the traced constant and the key was NOT
  replaced; K >= 1 after a pairing; **unknown 0**, else an untraced ROM call site; starved 0), `BT stopped`,
  `BT SSP BENCH autoconfirm off`, `BT XREC BENCH PIN off`, `BENCH end why=time ... pairs=5`, `BENCH LOGCAP ...
  lost=0`; in the ring `BENCH LOGFILE ... st=OK`. `BTBENCH.RUN` is gone from the card; `bt_bonds.bin` holds the
  laptop's address, `cassini.key` exists.
- Never in this run: `BT SSP pairing window refused`, `BT SSP key spent`, `BT SSP numeric comparison rejected,
  pairing window closed`, `BT XREC PIN unavailable`, `BT XREC CHAL refused: entropy not ready`, `BT fault`, `BT
  platform fault`, `BT shutdown: hard path`. (A failed laptop pairing **after** the autoconfirm, e.g. a timeout on the
  laptop side, spends the session's key by design: every later window of the run then refuses SSP. Restart the run.)

**8. Bond persistence** (second request, no window): put `printf 'secs=60\n' > $M/BTBENCH.RUN` on the card (step 2
without the other changes), `dfu` + `boot --timeout 160 &`, then `python3 tools/bt_sync.py --mac $MAC --list` inside the
60 s: no pairing (bond loaded from `SD:/bt_bonds.bin` before `esp_bluedroid_init()`, XREC key from `SD:/cassini.key`),
CHAL/AUTH ok. Its `BT ROM keys` line shows `installed 1` again (a fresh boot) with a different `public X` at entry.

**9. Checks that need a person at the device** (Home -> fourth tile (Bluetooth) -> ENTER starts BT mode; ENTER on the
BT screen opens a 60 s window, BACK leaves BT mode; while a prompt is up ENTER / BACK answer it):
- ENTER on the prompt accepts (`BT SSP numeric comparison accepted by the user, value NNNNNN`); BACK rejects (`...
  rejected by the user`, the laptop reports `Failed to pair`, the device stays in BT mode); a key in the prompt's
  first 0.5 s does nothing; `no` on the laptop while the prompt is up withdraws it; a window that ends during the
  prompt rejects it (`... window closed while waiting for the user`).
- The XREC PIN on the screen (`PIN NNNNNN` toast, a fresh DRBG value per window without the bench override) works for
  `--pair`. The toast must show the text, not a `...` pill (fixed for every image, BT=0 and BT=1; `make test-host`
  renders and checks it for both). ENTER again within the first
  ~50 s of a window keeps the same PIN.
- *Keys around the prompt:* `no` on the laptop and BACK on the device at the same moment: the device stays in BT
  mode (a BACK within 1 s after the prompt ended is swallowed); ENTER twice on the prompt: no new window, no new PIN.
- *Long peer name:* pair a phone whose name is longer than ~12 characters: the connected line shows `Connected ·
  <name>...` on one line (not cut after the first word).
- *Stale prompt:* slide REC on while a prompt is up, slide it off, open Bluetooth again: no old digits / "Pair? N s"
  while BT mode starts.
- *Idle current after BT* (if a meter is at hand): fresh boot idle on Home vs. after one BT session and back to Home;
  the mic PGA is now switched off again after the entropy collection (APLL still on, as after a playback).
- *REC slide right after entering BT mode:* the recording starts within ~0.2 s plus the RF calibration (the
  collection aborts on REC; log `BT entropy mic: skipped (REC switch on) ...` or the `aborted (REC switch)` status).
- *Recording after BT:* a remote recording started over XREC (`--rec-start`, `--rec-stop`) after the pairing records
  at the configured gain (the mic collection's gain 8 must not stick).
- *Decisive check that the ROM uses the RAM key slots* (one-off image, never shipped): in `bt_xrec_fw.c
  session_entropy_begin`, after `bt_p192_keygen`, stage a public key that does not belong to `d` (e.g. generate a
  second pair and stage `d` with the second pair's public key). Pairing (step 4) must then FAIL at the DHKey check
  (laptop: authentication failure). If it still succeeds, the ROM is not reading the slots this build replaces: stop
  and report. No key press needed with the bench trigger.

**10. Entropy assessment (NIST SP 800-90B; raises or confirms the 0.5 bit/symbol credit).** Run the block in "Entropy
source assessment (NIST SP 800-90B) — recipe for the lead" (Exp 2, above): 16 runs of `exp_entropy_dump.c` source 5
(`--arg 5`, 65,536 symbols each, `overruns=0`) and 63 runs of source 6 (`--arg 6`, 16 restart rows each),
`tools/bt_entropy_unpack.py seq|restart`, then `ea_non_iid -i -a -v mic_seq.bin 2` and `ea_restart -n -v
mic_restart.bin 2 <H_I>` (`-n`: non-IID; `-i` means IID in `ea_restart`). Repeat after a cold start, in a quiet room,
with speech near the mic and once at the usual recording gain. Decision rule and where to change the credit: same
section. Record the numbers there.

**Device log reference.** Refusals: `BT SSP numeric comparison rejected: pairing window closed`, `... rejected by the
user`, `... rejected: no answer within 30 s`, `... rejected: pairing window closed while waiting for the user`, `...
rejected: another one waits for the user`, `BT SSP prompt withdrawn: pairing ended by the peer` / `: peer
disconnected`, `BT SSP pairing window refused: <reason>` / `closed: <reason>` (reasons: `entropy source failed its
health test (until reboot)`, `entropy not credited`, `ROM P-192 key slots not as traced (key not replaced)`, `ROM
P-192 key not replaced (re-enter BT mode)`, `ROM RNG pool ran dry this session`), `BT XREC PIN unavailable: entropy
not ready (...)`, `BT entropy mic: skipped (recorder owns the ADC), no credit this session` (defensive only: BT mode
is never entered while recording), `BT entropy mic: skipped (REC switch on), no credit this session`, `BT entropy
mic: ... rejected: input at full scale (loud sound / saturation), frame not credited` / `... rejected: no analogue
noise in a frame, frame not credited` (sanity gate: no credit from that frame on; SSP refused unless an earlier
session of the boot seeded the DRBG), `BT SSP key spent: an accepted pairing failed; no SSP until the next BT-mode
entry` + `BT SSP pairing window closed: a failed pairing used this session's P-192 key`, `BT SSP numeric comparison
rejected, pairing window closed: <gate reason>`, `BT SSP numeric comparison rejected: pairing gate closed` (ENTER
after the gate failed), `BT SSP bond removed: Just Works / unauthenticated key`, `BT stored bond removed: not
MITM-authenticated`, `BT XREC close: peer not bonded with MITM`, `BT XREC CHAL refused: entropy not ready` followed
by `BT XREC close: entropy not ready`, `BT bonds not saved (SD write failed, retrying)` (retried every second, up to 8
times per session, and at BT stop). Faults: `BT bring-up failed, code N` (1 `bt_ctrl_open`, 2 HCI driver attach, 3
`esp_bluedroid_init`, 4 `esp_bluedroid_enable`: the HCI Reset .. init chain through the ROM), `BT fault, reason N` /
`BT shutdown: hard path, reason N` (`BT_XREC_FAULT_*` in `bt_xrec_bluedroid.h`: 2 bring-up timeout, 4 SPP config
timeout, 6 platform, 7 teardown deadline), `BT platform fault N` (1 `bt_ctrl_run()` stuck > 3 s, 2 `bt_ctrl` fault,
3 HCI command timeout), `BT bt_ctrl fault, first code N`. The hard path reboots and leaves fault record `0xFA0170B1`
(`dev_cycle.py` "previous fault"; `AON_BACKUP(1)` = reason); BT stays refused for that boot.

**Capture on failure:** the ring log (before anything else logs), `BTBENCH.LOG`, `/tmp/bench.btsnoop` (laptop HCI),
the `bluetoothctl` transcript, `SD:/bt_bonds.bin` and `SD:/cassini.key`, the `dev_cycle.py status` output (previous
fault), and the BT screen state if someone can see it. A first failure in pairing or connection set-up is most likely
a ROM message the host has not seen yet: compare the laptop's btmon trace with `docs/BT_ROM_HCI_MAP.md` §10 (which ROM
reply or event each step needs) and check the opcode-table rows of the commands around the failure. If the laptop
pairs but its RFCOMM connect is then refused or re-paired, suspect the SPP server's `ESP_SPP_SEC_MITM` flag first
(esp_spp_api.h advises against it "due to certain limitations"; the host harness runs bonded, re-paired and Just
Works peers with it). Per the hardware-access rule, leave the latest BT-branch build on the device afterwards
(preferably the release `make -C open_fw BT=1` image: a bench image honours `SD:/BTBENCH.RUN`).

## Headphones (BT=1) on silicon: how to test

First silicon test of headphones mode (A2DP source + AVRCP, `docs/BT_A2DP_DESIGN.md`; implementation record and
deviations: `docs/BT_A2DP_HP_SESSION.md`). First silicon runs with a laptop as the sink: session 7 above. The host runs the same session end to
end against a fake headset (`make -C open_fw test-bt-host`, scenario S: `test_bt_host_hp hps`, steps S1-S10). The
laptop is not involved: the lead flashes and reads logs over USB, the user handles the headphones (and the device's
keys for the UI part). Exp numbers refer to design §11.

### The image

**Build the bench image with the headphones rows forwarded**: `make -C open_fw BT=1 BENCH=1 HP_GATES=1` (needed:
the rows below, and the HCI trace notes `BT HCI trace ...` are only in these images). Release
`BT=1` images still **refuse** `0401`/`0402`/`0405`/`0408` (inquiry, cancel, create connection, cancel connection),
so in them headphones mode starts but can neither search nor connect: `Search` and picking a device fail at once with
the footer `Not in this build` and `BT hp discovery refused=this build's bt_ctrl refuses inquiry/page ...` (no
reconnect attempts either; `BT hp ready ... hp_rows=0`). **Tomorrow's / any headphones test image must be built with
`BT=1 BENCH=1 HP_GATES=1`** (`BT hp ready ... hp_rows=1`). Each row flips to forwarded only after its silicon
experiment passed (design §7). The bench image runs the UI normally as
well: the `SD:/BTBENCH.RUN` trigger is optional.

What the image does:
- **Bluetooth tile** (home, 4th tile): a choice `Phone (sync)` / `Headphones`. Phone is the unchanged XREC mode (it
  stops headphones mode first: one controller owner). Headphones opens the headphones screen and starts headphones mode
  (`BT hp start`), which reconnects to the last headset by itself (backoff 0, 3, 10, 30, 60 s, then every 60 s).
- **Headphones screen** (Settings layout; LEFT/RIGHT move, ENTER selects, BACK leaves and **keeps the session running**
  so the player can use it): `Headphones mode On/Off`, status (`<name>  48k bp35` when connected, `Connecting...`,
  `Pairing...`, `Reconnecting  N s`, `Not connected`), `Disconnect`, `Search for headphones` (10 s inquiry, then the
  name pass), `Forget <name>`, then one row per device found (`name` or address, `-NN dBm` or `audio`/`other`).
  ENTER on a device opens a 120 s pairing window **for that device only** and connects; Just Works is accepted only
  there (patch 0036 hook, entropy gate, ROM key replaced). Footer/toasts: `Headphones connected`, `Paired`,
  `Pairing failed`, `Pairing refused`, `Connection failed`, `Headphones disconnected`, `Pairing unavailable` (gate).
- **Settings → Audio output**: `Speaker` (default, as before), `Headphones` (the player plays to the headset only; with
  no headset connected a file does not start and shows `Headphones not connected`), `Both` (DAC and headset, the DAC
  delayed by the headset latency estimate). Stored in `SD:/bt_hp_cfg.bin` with `bt_vol` and the last headset, not in
  `settings.bin` (design §6.6).
- **Both, as the user sees it**: with a headset connected and streaming-ready when the file opens, the file plays on
  the headset and on the speaker (or the wired jack) together, the speaker held back to match the headset. With no
  headset at that moment the file plays on the speaker alone at the speaker volume (no toast); a headset that connects
  later is used from the next file. Mono and stereo files both work (the headset gets mono as mono). One volume for
  both outputs: the headset's `bt_vol` (VOL keys and headset buttons change it; with AVRCP absolute volume the headset
  applies it and the speaker follows the same level). The speaker amp follows the jack as in Speaker mode, and pulling
  the jack pauses both. If the app is held up long enough for the speaker to run dry, the speaker skips the missed
  part to stay in step with the headset (the headset is not interrupted). If the headset drops mid-file the player
  pauses (if headphones mode itself stopped, Play continues on the speaker; after a lost link reopen the file). If the
  PCM ring had to borrow the DAC buffer (no heap), Both plays on the headset only.
- **Player** with Headphones (Both: speaker side above): speaker amp off, jack plugging ignored, volume = headset
  volume (AVRCP absolute volume when the headset supports it, else PCM gain; a volume change on the headset is
  followed by the device's VOL keys); headset buttons PLAY and PAUSE toggle (one-button headsets guess which one to
  send), STOP pauses, NEXT/PREV skip or seek like the device's arrow taps, VOL+/- step the volume once (`BT hp avrcp
  key code=.. rel=0 key=N` logs each decision); a link loss, credit stall, START failure or the headset pausing itself
  (in-ear detection) pauses the player at the last heard sample. Buttons pressed and headset events while no file
  plays are dropped when the next file starts.
- **Exit paths**: power-off, USB disk mode and phone mode stop headphones mode first; the REC switch suspends the stream
  (link kept).

### The bench trigger (`mode=hp`)

Same one-shot file as phone mode (`SD:/BTBENCH.RUN`, read and deleted at boot; strict parser, `bt_port/bt_bench.h`).
With `mode=hp` the boot opens the headphones screen and the run drives the session for `secs`, then stops BT and
writes `SD:/BTBENCH.LOG` (8 KiB head of the log; keep runs short: `secs=60..180`). Stages:

| `hp_stage` | Does | Exp |
|---|---|---|
| `scan` | `hp_secs` s inquiry + name pass; logs every device (`BENCH hp dev i=.. addr=..:..:.. cod=.. audio=.. name=..`) | 4a |
| `connect` | picks `hp_addr`, or `hp_pick=first` (a scan, then the first device of major class Audio/Video, headset/headphones/speaker types first; phones and PCs that only set the Audio service bit are never picked), or `hp_pick=last` (the headset stored by an earlier stage: no scan, no pairing); opens its window if `hp_pair=1`, connects, holds the link; stats every second for 15 s, then every 15 s | 4b |
| `tone` | as `connect`, then streams a 16 kHz mono tone through the full output path (`aout` -> ring -> x3 resampler -> SBC): `hp_tone_hz=1000` (default) or `hp_tone_hz=0` (440/880 Hz alternating each second) | 4c |
| `player` | as `connect`, then plays `hp_file` (or the first recording of the file list) once in the player, Audio output forced to Headphones for that file; at `secs` the run injects BACK into the player (`BENCH hp play: time limit, BACK`), so a long file ends with the run | 4d |

`hp_pin=1` (any connect stage, diagnostic): answer a **legacy** PIN Code Request from the picked device inside its
pairing window with `0000` (`BT hp legacy PIN pairing answered 0000 ...`). Not needed since patch 0040 (session 7);
use it only if a headset pairs with legacy PIN.

Examples (`AA:BB:CC:DD:EE:FF` = the headset address as a phone shows it; with `hp_pick=first` no address is needed):
```
mode=hp hp_stage=scan hp_secs=10 secs=40
mode=hp hp_stage=connect hp_pick=first hp_pair=1 hp_secs=10 secs=90
mode=hp hp_stage=tone hp_pick=first hp_pair=1 secs=120
mode=hp hp_stage=tone hp_addr=AA:BB:CC:DD:EE:FF secs=120 hp_tone_hz=0
mode=hp hp_stage=player hp_pick=last secs=180
mode=hp hp_stage=tone hp_addr=AA:BB:CC:DD:EE:FF secs=120 hp_rate=44100 hp_bitpool=28
mode=hp hp_stage=tone hp_pick=last secs=120 hp_speech_bp=0
```
The tone (and any 16 kHz recording in the player stage) goes out at the speech bitpool (`hp_speech_bp`, default 24)
while `bp=` prints the negotiated maximum: the per-second line's `bp_eff=` is what is on air. A throughput run at
the max bitpool (the H3 gate, a bitpool sweep) needs `hp_speech_bp=0`, as in the last line.
`hp_pair=1` is needed once per headset (first pairing, or after `Forget`); afterwards the stored key in
`SD:/bt_hp_bonds.bin` reconnects without pairing. In bench runs the pairing window is opened by the run instead of the
user (logged `BENCH hp pair window note=bench, no user...`); the policy is otherwise the same.

### Recipe

```bash
B=/tmp/sl6801_openfw_bt_hp_bench
# 1. Build + flash (update-up writes dumps/firmware_2M_openfw.bin: build right before flashing)
make -C open_fw BT=1 BENCH=1 HP_GATES=1 BUILD_DIR=$B update-up        # 0 warnings, "Packed hybrid FIRM ..."
sudo env OPENFW_FIRM_BIN=$B/open_fw_firm.bin python3 tools/dev_cycle.py flash
# 2a. ONCE, before the first stage only: clean headphones state (device in USB disk mode)
udisksctl mount -b /dev/sda1; M=/media/$USER/<label>
rm -f $M/bt_hp_bonds.bin $M/bt_hp_cfg.bin
# 2b. Every stage: one request (keep bt_hp_bonds.bin / bt_hp_cfg.bin: later stages reconnect with the stored key)
rm -f $M/BTBENCH.LOG
printf 'mode=hp hp_stage=scan hp_secs=10 secs=40\n' > $M/BTBENCH.RUN
sync && udisksctl unmount -b /dev/sda1
# 3. The user puts the headphones in PAIRING mode (usually: hold the power button until the LED flashes
#    blue/red), within a metre of the recorder. Then reboot into the run and wait for USB to come back:
sudo python3 tools/dev_cycle.py dfu
sudo python3 tools/dev_cycle.py boot --timeout $((40 + 60)) > /tmp/hp_boot.txt 2>&1
# 4. Read SD:/BTBENCH.LOG (USB disk mode again) and the ring log (dev_cycle.py log) and report.
```
Then repeat 2b-4 (never 2a: deleting `bt_hp_bonds.bin` between stages loses the pairing, and the next stage's
reconnect is then refused as `Just Works pairing refused`) with the next stage: `connect` (`hp_pick=first hp_pair=1`,
headphones still in pairing mode), `tone` (`hp_pick=last`, the user listens: a steady 1 kHz tone, then the alternating
one with `hp_tone_hz=0`), `player` (`hp_pick=last`). Logs print only the last 3 address bytes, so stages after
`connect` use `hp_pick=last` rather than `hp_addr`. If `scan` does not list the headset, the rest cannot run: report
the scan log.

**Manual UI check (no request file, user at the device):** Bluetooth tile → `Headphones` → `Search for headphones`
(headphones in pairing mode) → ENTER on the headset row → `Pairing...` → `Headphones connected` → BACK → Settings →
`Audio output` → `Headphones` → BACK → Files → play a recording: audio in the headphones; try the headset buttons
(play/pause, next/previous, volume), the device's VOL keys, a seek, pause/unpause, then switch the headphones off
(player pauses: `Headphones disconnected`) and on again (reconnects within the backoff; PLAY resumes). Then Settings →
`Both`, and power the recorder off and on: the last headset reconnects when the Bluetooth tile's `Headphones` is
opened again (headphones mode is not started at boot).

### Expected log lines (`BT hp ...`, `BT HCI ...`, `BENCH hp ...`)

- Bring-up: `BT hp start (Bluedroid, headphones) name=Cassini_1D last=0 out=0`, `BT hp a2dp prof state=1`,
  `BT hp seps sep0=0x03 sep1=0x02 max_bp=35 err0=0 err1=0`, two `BT hp sep registered seid=N st=0`,
  `BT hp ready name=Cassini_1D last=0 gate=ok` (`gate=<reason>` = pairing will be refused: report it).
- Scan: `BT hp discovery secs=10 inq_len=8 err=0`, `BT HCI inquiry st=0x00 rom` (the ROM accepted `0401`),
  `BT HCI inquiry complete st=0x00 results=N`, `BT HCI name request` / `name complete` per device,
  `BT hp found peer=..:XX:XX:XX cod=0x...... audio=1 name=<headset>`, `BT hp discovery stopped found=N`.
- Connect: `BT hp pick ...`, `BT hp pair window open ... gate=ok`, `BT hp a2dp connect ... err=0 inq=0 scan=0x00`,
  `BT HCI create connection st=0x00 rom` (`0405` accepted), `BT HCI connection complete h=0x0080 st=0x00`,
  `BT hp acl up`, `BT hp Just Works pairing accepted ...`, `BT hp auth ... st=0 key=4|5|7|8`, `BT hp codec cfg
  cie=0x........ ok=1 rate=48000 mode=3 blocks=16 sb=8 bp=2-35`, `BT hp a2dp conn state=2 ... mtu=NNN cfg_ok=1`,
  `BT hp linkinfo role=M|S pkt_type=0x0000 req_pt=0x.... allow_rs=0|1 max_slots=N role_chg=N out=1 acl=0x0080
  edr=0..3 slots35=0..3`, `BT hp avrcp ct conn=1 hdl=N caps_err=0`, `BT hp avrcp rn caps ... abs_vol=0|1`,
  `BENCH hp connected t_ms=... rate=48000 bp=35`. `pkt_type` stays 0 (only a `040F`, which stays REJ, makes the
  controller report packet types); what was asked for is `req_pt` (our `0405` Packet_Type) and what the headset can
  do is `edr` (page-0 bits: 1 = 2 Mb/s, 2 = 3 Mb/s, 3 = both) and `slots35` (1 = 3-slot, 2 = 5-slot, 3 = both).
  `role=S` with `role_chg>=1` = the headset took the master role (also when it switched before the Connection
  Complete, as TWS earbuds do).
- Tone: `BT hp media START err=0`, `BT hp media ack cmd=2 st=0`, `BT hp audio STARTED ours=1 tick=1`,
  `BT hp stream rate=48000 bp=35 bp_min=.. speech_bp=24 fpp=N mtu=NNN tick_ms=20`, then per second
  `BENCH hp t=N link=3 rate=48000 bp=35 bp_eff=24 fpp=N pkts=~37..49 Bps=~22900 sbc=~375 acked=~375 pads=0 lost=0
  drop=0 allocf=0 cs_no=0 txq=0..4 txq_max=.. down=.. nocp_max_ms=.. stalls=0 late=..` (`bp_eff=35 Bps=~31100`
  with `hp_speech_bp=0`; `Bps` = SBC payload bytes/s at `bp_eff`) and `BENCH hp linkinfo role=M pkt_type=0x....
  max_slots=5 mode=0 acl_sent=.. acl_done=.. heap_free=.. heap_min=..`. Healthy: `sbc` ~375/s at 48 kHz (~345 at
  44.1), `acked` close to `sbc`, `pads=0`, `lost=0`, `nocp_max_ms` well under 800, `stalls=0`, `heap_min` > 8000.
  A REC switch while STARTED logs `BT hp media SUSPEND`; a headset that refuses it logs `BT hp SUSPEND refused
  retry=1 n=..` and is asked again up to 3 times, 1 s apart.
- HCI trace (`HP_GATES` bench images only): `BT HCI trace ev=0xEE st=0xSS [op=0xOOOO]` for every event to the host
  (event code, its first parameter byte or the command's status, the opcode of a CC/CS), up to 64 queued; NOCP,
  inquiry results and successful CCs outside link control are skipped. Pairing as initiator should read `op=0x0411`,
  `ev=0x17`, `op=0x040C`, `ev=0x31` (SSP), `op=0x042B`, `ev=0x32`, `ev=0x33`, `op=0x042C`, `ev=0x36`, `ev=0x18`,
  `ev=0x06 st=0x00`; `ev=0x16` after `040C` = legacy PIN (the ROM decided legacy: `docs/BT_ROM_HCI_MAP.md` §12).
- End: `BENCH exit why=time`, `BT hp teardown ms=NNN profile_ms=NN job_rc=0 reason=0` (ms < 2000; ~217 idle,
  ~650 with the A2DP link up, session 7; `job_rc=5` =
  A2DP deinit not confirmed in time), `BT hp stopped`, `BENCH end`, `BENCH LOGFILE ... st=OK`.

**Failure signatures to report verbatim:** `BT HCI inquiry st=0x..` other than 0x00 (ROM refused `0401`; LM state),
`BT HCI create connection st=0x0C` (`0405` refused: inquiry/page state), `connection complete st=0x04` (page timeout:
headset not connectable / out of range), `BT hp Just Works pairing refused` (window/gate), `BT hp auth ... st=` non-0
(pairing failed: also `BT hp SSP key spent`), `BT hp legacy PIN pairing rejected` (the ROM chose legacy pairing), `BT hp connect failed`, `BT hp connect timed out` (and any
`late a2dp connect closed`), `BT hp codec cfg ... ok=0`, `BT hp media ack cmd=2 st=1|2`, `BT hp START failed, retry`
/ `BT hp START failed twice` (a play press 2 s later tries again: `START: new demand ...`), `BT hp credit stall` (link
stopped acknowledging), `BT hp stall: no Disconnection Complete after the 0406 then=restarting headphones mode` (the
ROM kept the dead link: headphones mode stops and starts again by itself, `restart after the stall escalation`, then
reconnects to the headset; at most once per 5 minutes, else `then=headphones mode off`), `linkinfo ...
max_slots=1` (1-slot packets: throughput too low, design §7 role-switch risk), `pads` > 0 or `nocp_max_ms` > 200
(throughput), any `BT hp fault reason=N`, `BT bt_ctrl fault`, `BT platform fault`, a reboot (`PREV_FAULT`).

**What to report:** the headset make/model, `BTBENCH.LOG` of each stage, the ring log, what the user heard (clean
tone / crackles / dropouts, delay of play/pause), whether the headset announced "connected"/"paired", and for the UI
run which steps worked. Afterwards leave the latest BT-branch build on the device (per the hardware-access rule;
the bench image is fine for continued headphones tests, a release `BT=1` image cannot connect yet).

## Exp 5 (plan step 8): power, USB, card and REC transitions with BT active — how to test

Plan §5 M5 "Exp 5": every way the device can leave or lose Bluetooth while phone or headphones mode runs must end in
the **coordinated teardown** (`bt_shutdown()`: graceful within 2 s, else the M1 hard path with the recording
finalised and fault record `0xB1`), never in a hang, a WDT reset or a damaged card. What each transition does in the
code (2026-11-25, branch `feat/bt-step8`; host-tested where the harness reaches, nothing of it on silicon yet):

| Transition | Phone mode (BT screen) | Headphones mode (runs in the background) |
|---|---|---|
| Power off (hold the power key) | `ui_poll_power_button`: `bt_shutdown()` then power-off | same; on the player screen `aout_stop()` first |
| USB cable plugged | stays in BT mode (charges, no USB disk); USB disk mode starts after BACK | home/browser: auto USB disk mode, which runs `bt_shutdown()` first (no `bt_hp_bonds.bin` write while the host owns the card); player / BT screens: after leaving them |
| Settings → USB disk | not reachable from BT mode | `bt_shutdown()` first, as above |
| microSD removed | no detection (plan §6.7 dropped "card removal"); XREC file commands fail on the card, the session keeps running; BACK leaves normally | bond flushes fail: `BT bonds not saved (SD write failed, retrying)`, retried up to 8 times, then at the next change / stop |
| REC switch on | leaves BT mode (`bt_xrec_stop()`), recording starts | stream suspended, link kept (design §6.9), recording starts |
| Low battery (new) | below 3450 mV in 3 samples 0.5 s apart, no VBUS: leaves BT mode, toast `Low battery: BT off` | stopped (`bt_hp_stop()`), same toast; on the player the file pauses with that toast |
| Starting BT at low battery (new) | refused with that toast (one sample < 3450 mV, or still latched below 3550 mV) | refused, same |
| XREC REBOOT / OTA / DFU | deferred reboot: `bt_shutdown()` first, then reboot / SD OTA / USB DFU (recording finalised) | n/a |
| A Bluedroid task stuck (new) | WDT supervisor: hciT/BTU/BTC ping unanswered 8 s → `BT task stalled: <task>`, platform fault 4 → graceful recovery or hard path; ping unanswered 20 s → the WDT is no longer fed (reset ~32 s later) | same (`BT hp fault`) |

**Image:** a bench image, because headphones mode needs the forwarded rows (`HP_GATES=1`); no `SD:/BTBENCH.RUN` on
the card, so it boots into the normal UI and everything below is done by a person at the device.
```bash
B=/tmp/sl6801_openfw_bt_exp5
make -C open_fw BT=1 BENCH=1 HP_GATES=1 BUILD_DIR=$B update-up      # 0 warnings, "Packed hybrid FIRM ..."
sudo env OPENFW_FIRM_BIN=$B/open_fw_firm.bin python3 tools/dev_cycle.py flash
MAC=02:CA:55:00:01:1D
```
Before starting: the laptop is paired with the device in phone mode (phone-mode recipe steps 4-5, or the UI: BT tile →
Phone → ENTER → accept the prompt), a headset is paired in headphones mode (headphones recipe, UI check), and the card
holds a few recordings including one of several MB. After **every** item: `sudo python3 tools/dev_cycle.py log >
/tmp/exp5_<item>.txt` (ring log; USB disk mode) and note what the screen showed. Items that power the device off read
the log after the next boot. In every log check: no `BT shutdown: hard path`, no `PREV_FAULT=` line at the next boot
(a WDT or fault reset), teardown lines under 2 s.

**E1 Power off in phone mode.** (a) BT screen idle, laptop not connected; (b) laptop connected and authorised
(`bt_sync.py --mac $MAC --ping`, then within 10 s); (c) during a long transfer: `python3 tools/bt_sync.py --mac $MAC`
(syncs all recordings) and hold the power key while it runs; (d) while a pairing prompt is up (laptop: `bluetoothctl
remove $MAC`, then pair again and hold the power key on the prompt). Expect each time: `BT SSP prompt withdrawn: BT
stopping` (d only), `BT mem stop ...`, `BT sup stop stalls 0 ...`, `BT stopped`, the device switches off. Power on:
BT tile → Phone works, the laptop reconnects with its bond (a, b, c) and the transfer can be repeated; after (c)
`fsck.exfat -n` on the card (USB disk mode) reports no error.

**E2 Power off in headphones mode.** (a) headphones on, home screen, headset connected, nothing playing; (b) a
recording playing on the headset; (c) headphones screen while `Searching...`. Expect `BT hp teardown ... reason=0`
(ms < 2000), `BT hp stopped`, off. Power on → BT tile → Headphones: reconnects to the headset.

**E3 USB cable.** (a) phone mode on the BT screen, laptop connected: plug the cable. Expect: BT mode stays (the
charge icon), no USB disk; transfers still work; BACK → USB disk mode comes up by itself within ~2 s, with `BT stopped`
logged before it. (b) Headphones on, home screen, headset connected: plug the cable. Expect `BT hp stopped` logged
**before** the USB disk mode starts; on the laptop the card mounts and `fsck.exfat -n` is clean; unplug → home; BT tile
→ Headphones reconnects. (c) Playing on the headset, plug the cable: playback goes on; leave the player → USB disk
mode as in (b). (d) Settings → USB disk with headphones on: as (b).

**E4 microSD removal.** (a) Phone mode, laptop connected and authorised: pull the card, then `bt_sync.py --mac $MAC
--list` and `--stat`. Expect error answers (or an empty list), no reboot, the BT screen responsive; BACK leaves BT
mode (`BT stopped`); insert the card and report whether the home screen sees it again (the home loop re-probes only
a card whose init failed; otherwise a power cycle is needed); then BT tile → Phone: the laptop reconnects. (b) Headphones on and connected: pull the card, then
use the headphones screen `Forget` on the headset (forces a bond-file write). Expect `BT bonds not saved (SD write
failed, retrying)`, at most 8 retries, no reboot, headphones mode keeps running. Re-insert the card, BACK, power off
and on: the old headset bond is either still there or gone (whichever write last reached the card), never a damaged
`bt_hp_bonds.bin` (`fsck.exfat -n` clean, headphones mode starts). (c) Card out, enter phone mode, card in, leave:
no crash; report what the screen showed.

**E5 REC switch.** (a) Phone mode during a `bt_sync.py` transfer: slide REC on. Expect `BT stopped` within 2 s,
recording starts at once, the laptop sees the link drop; REC off: the recording is saved and plays back without a gap
at its start. (b) Headphones streaming: REC on. Expect `BT hp media SUSPEND` (or `SUSPEND refused retry=N`), the
headset falls silent, recording runs; REC off, back to the player, PLAY: the stream resumes. Report any hum or noise
in that recording (Exp 4h).

**E6 Low battery.** Needs VBAT near 3.45 V: either let the battery run down with BT active (the `BT vbat mV last N min
M vbus 0` line every 30 s while BT runs shows the level and the sag under RF load), or, if the bench has one, a lab
supply in place of the battery at 3.40-3.60 V. Unplug USB (VBUS cancels the rule). (a) Phone mode below 3450 mV:
expect `BT low battery mV N` after ~1.5 s, `BT phone mode left: low battery`, `BT stopped`, toast `Low battery: BT
off` on the home screen. (b) BT tile → Phone again: refused at once (`BT start refused, low battery mV N`, same toast).
(c) Headphones playing, below 3450 mV: `BT headphones stopped: low battery`, the player pauses with the toast. (d)
Raise to >= 3550 mV (or charge): BT starts again. Report the `min` values seen while BT was busy (pairing, transfer,
streaming) against the idle `last`: if the radio alone pulls a healthy battery under 3450 mV, the threshold or the
sample count needs changing (`open_fw/bt_port/bt_lowbatt.h`).

**E7 WDT supervisor on silicon.** Read the `BT sup stop stalls 0 hciT n=.. max=.. ref=.. out=..` line written at every
BT stop of E1-E6: `stalls` must be 0, `max` (ms, the slowest ping answer) far below 8000 and `ref` (refused posts)
0. (a) Idle bonded phone: phone mode with the laptop connected and nothing sent for 10 min: no `BT task stalled`, no
fault. (b) Stalled transfer: start `python3 tools/bt_sync.py --mac $MAC` on a large recording, then freeze the client
for 60 s (`kill -STOP <pid>`, later `kill -CONT <pid>`): no `BT task stalled`, the transfer continues or the client
times out; BACK leaves cleanly. Report `max` for both: it sets how much margin the 8 s stall limit has
(`open_fw/bt_port/bt_task_sup.h`).

**E8 Recovery images.** (a) Phone mode, laptop connected and authorised: `python3 tools/bt_sync.py --mac $MAC
--reboot`. Expect `BT XREC deferred reboot, mode 1`, `BT stopped`, then a normal boot. (b) The same with `--ota
dumps/update_openfw.up` (written by the `update-up` above; the bench image itself is fine): `BT XREC deferred reboot, mode 2`,
`BT stopped`, the SD OTA updater runs and the device comes back with the image. (c) Start a recording over BT
(`--rec-start`) and then `--reboot`: the recording is finalised before the reboot (plays back, correct length). (d)
After all of the above `sudo python3 tools/dev_cycle.py dfu` and a re-flash still work (the USB DFU recovery path).

**What to report:** per item the ring log (and `SD:/BTBENCH.LOG` if one was written), what the screen and the headset
did, `fsck.exfat -n` results, any reboot or `PREV_FAULT`, and the `BT vbat` / `BT sup` lines. Afterwards leave the
latest BT-branch build on the device.

## Sync during recording: M2/M3 measurement recipe

Stage S2 of `docs/BT_SYNC_SPEED_AND_RECORDING.md` (section 3): measure whether a radio that is up, linked or
transferring changes a recording, with **today's** firmware (phone mode on the BT screen, recording started over
`+XREC:RECSTART`), before any S3 hardening. The result selects the fallback ladder level (design 2.6) and is the
decision point with the user. Nothing here has run on silicon yet (written 2026-11-25, branch `feat/bt-sync-measure`).

### What the bench image measures (F0)

`make -C open_fw BT=1 BENCH=1` adds counters to the recorder and phone mode; every other image (BT=0, release BT=1,
`HCI_PROBE`, `DEV_EXP`) is byte-identical to the image without them (`open_fw/include/openfw_bench_meas.h`,
`src/bench_meas.c`, hooks in `src/recorder.c`, `src/xrec_proto.c`, `bt_port/bt_bench_fw.c`). Per recording (reset
at every REC start, also for recordings started with the REC switch and with no radio at all):

| field | meaning | M2/M3 use |
|---|---|---|
| `file`, `seq`, `ch`, `gain`, `dsp`, `vor` | the first WAV of the recording and its settings | map counters to WAVs; check settings |
| `dma_smp`, `clk_smp`, `drift` | DMA blocks x 2048 vs us_timer x 16 kHz since capture start; drift = dma - clk | M2(e) length, M3 sample accounting |
| `wav_smp`, `ov`, `comp`, `lost`, `acct` | frames in the WAV, ring overruns, zero blocks written for them, ov - comp, (dma - ov + comp) x 2048 - wav | M2(e) overruns = 0; M3 accounting exact (acct = 0 at stop, VOR off) |
| `gap_max_us`, `poll_max_us` | largest gap between two `recorder_poll()` calls (the app-loop gap the 384 ms ring must cover), longest poll | M3 "max recorder-loop gap" |
| `wr_max_us`, `wr_n`, `ckpt_max_us` | longest SD write of one 128 ms block, writes, longest WAV checkpoint | M3 SD latency |
| `gets_b` | GETS payload bytes sent during the recording | M2 C3 load, M3 cap check |
| `idle_min_pm`, `cpu_n` | lowest 1 s idle CPU (permille) during the recording, windows seen (bench phone-mode runs only) | M3 "idle >= 20 %" |

Where they appear:
- **Ring log, every REC stop:** `BENCH rec seq=.. file=REC_NNNN.wav rec=0 ch=.. gain=.. ... cpu_n=..` (read with
  `dev_cycle.py log` in USB disk mode; the ring is SRAM: read it before a power cycle).
- **`+XREC:DBG`** (bench images, after AUTH; `docs/XREC_PROTOCOL.md` 5.4): `+XREC:DBG,LIVE,<fields>,gets_tot=..,
  gets_fr=..,idle_pm=..,app_pm=..,btc_pm=..,cpu_ms=..` for the running recording, `+XREC:DBG,REC,<i>,<fields>` for
  the i-th latest finished one (8 kept). Laptop: `python3 tools/bt_sync.py --mac $MAC --dbg` / `--dbg-rec 0`.
- **Phone-mode bench runs (`SD:/BTBENCH.RUN`):** with every `BENCH status` line (30 s): `BENCH cpu ms= idle_pm=
  app_pm= btc_pm= rec=`, `BENCH tasks <task>=<permille> ... other=` (per-task CPU over the 30 s, the headphones bench
  machinery), `BENCH meas <rec fields>` and `BENCH meas+ <cpu fields>`. `SD:/BTBENCH.LOG` ends with `BENCH final
  gets_tot=.. ... recs=N` and `BENCH final rec i=live|0|1 <fields>` (the capture buffer keeps the head of a long run,
  this trailer always survives).

Laptop side (`tools/bt_sync.py`): `--bench-load SECONDS` holds one link for SECONDS and logs a `+XREC:DBG` line every
`--dbg-every` s (0 = only at the end); `--load-rec` sends RECSTART first and RECSTOP last (then logs `DBG,REC,0`);
`--load-file NAME` pulls GETS bursts of that `/record` file in a loop (`--load-kbps N` caps the average in KiB/s,
0 = as fast as the link goes; `--burst-size`, `--chunk-size` as for a sync); `--load-storm S` adds STAT + LS + a DEL
of a name that does not exist every S s (directory scans, nothing deleted); `--dbg-log FILE` appends every line.

Analysis: `tools/rf_audio_check.py` (numpy; `--self-test`, also run by `make -C open_fw test-host` and
`tools/test_tools.py`) implements the M2 metrics (a)-(e) against the C0 repeats and prints a Markdown table (`--md`,
`--json`); `--bench-log` adds the device counters per WAV (length vs `clk_smp` within +-2048, samples = `wav_smp`,
`ov` = `lost` = `acct` = 0). Exit status 1 = a capture failed. Thresholds are the design's (`--lim-*` to change).

### Roles

- **Agent:** builds and flashes, prepares the card, sets the audio settings over USB, writes `BTBENCH.RUN`, runs the
  laptop schedule (bluetoothctl, `bt_sync.py`), reads the ring log, pulls WAVs and logs over USB MSC, runs
  `rf_audio_check.py`, writes the results below. Before every step that needs the user it says what will happen,
  how long it takes and what may and may not be touched.
- **User:** the physical steps: device in the foam box at a fixed place (mic side up, nothing touching it), the
  speaker for the tone, the USB cable (plug / unplug when asked), the REC switch, the power key, the headset for C5,
  covering the mic for Cm; quiet room during captures; the ABX listening test (f). Never touch the device or the
  cable during a capture.

Fixed for every capture: **USB cable unplugged** while recording (VBUS charging is its own noise source and USB disk
mode would take over the Home screen); laptop at 1 m; DSP `2` (BYPASS) for the metrics, then one block with DSP 1
(Voice Clarity) for listening; VOR off (`acct` is only exact without VOR); 75 s per capture, analysed 7.5 s in from
each end (the 60 s window; RECSTART / RECSTOP and C1's connects happen outside it). Signals: **S** silence (speaker
off), **T** a **997 Hz** sine from the external speaker at about -20 dBFS (adjust once per gain from the first C0 T
capture: the report's header prints `tone ... at X dBFS`). Not 1000 Hz: a lost 2048-sample block is exactly 128
periods of 1 kHz and leaves no phase step; at 997 Hz it is a 2.4 rad step.

### Setup (once)

```bash
MAC=02:CA:55:00:01:1D
S=/home/silvio/dev/.build-scratch/s2/run          # bench build dir (not /tmp: small RAM disk)
make -C open_fw BT=1 BENCH=1 BUILD_DIR=$S OUT_BIN=$S/firmware_2M_openfw.bin OUT_UP=$S/update_openfw.up update-up
sudo env OPENFW_FIRM_BIN=$S/open_fw_firm.bin python3 tools/dev_cycle.py flash    # 0 warnings above; ~1 min
```
The laptop must already be bonded and XREC-paired with the device (phone-mode recipe above, steps 1-5; the bond and
`SD:/cassini.key` survive a reflash). Card (USB disk mode; `M` = its mount point): a 20 MB GETS object and no
leftovers:
```bash
udisksctl mount -b /dev/sda1; M=/media/$USER/<label>
head -c 20000000 /dev/urandom > $M/record/LOAD.WAV; rm -f $M/BTBENCH.RUN $M/BTBENCH.LOG
sync; udisksctl unmount -b /dev/sda1
sudo python3 tools/openfw_usb.py --audio-cfg > $S/user_audio_cfg.txt     # the user's settings, restored at the end
```

### M2 — one block per setting (about 40 min each)

Blocks, in this order (most sensitive first; stereo blocks may go to a second session): gain 31 mono S, 31 mono T,
20 mono S, 20 mono T, 31 stereo S, 31 stereo T, 20 stereo S, 20 stereo T; then **Cm** (gain 31 mono S, mic covered:
C0 x1 + C2 x1) and one **listening** block (gain 31 mono, DSP 1, speech or the tone: C0 x1 + C3 full speed x1).
Each block, with `G` = gain, `CH` = 1 or 2, `D=$S/m2/g${G}_ch${CH}_<S|T>`:

**A. Fresh boot, radio never on: C0 x2** (agent, cable in; then user, ~4 min):
```bash
mkdir -p $D
sudo python3 tools/openfw_usb.py --reboot normal      # fresh boot: the radio has not been on in this boot
sleep 8                                               # back in USB disk mode (cable in)
sudo python3 tools/openfw_usb.py --set-dsp 2 --set-gain $G --set-ch $CH --set-vor 0 && sudo python3 tools/openfw_usb.py --audio-cfg
```
Settings set over USB are applied at once but saved to the card only when USB disk mode ends by unplugging (a USB
reboot does not save them): so the reboot comes first. User: unplug the cable (the settings are saved, Home shows),
then twice: REC switch on, 75 s (the agent counts), off, wait 10 s. For T blocks the speaker plays the tone
throughout. Cm block: the mic is covered for both.

**B. One BT session: C1, C2, C3 (48 KiB/s cap) and C3 (full speed), x3 each in random order** (~20 min, user hands
off). Agent, cable in, then the user unplugs once the BT screen shows:
```bash
sudo python3 tools/dev_cycle.py log > $D/ring_A.log                  # BENCH rec lines of the C0 captures
udisksctl mount -b /dev/sda1 && printf 'secs=1500 exit=auto\n' > $M/BTBENCH.RUN && sync && udisksctl unmount -b /dev/sda1
sudo python3 tools/openfw_usb.py --reboot normal                     # boots into BT mode (bench run, 25 min)
sleep 20                                  # BT mode up (entropy + bring-up); user: unplug when the BT screen shows
L=$D/dbg_B.log; for c in $(printf '%s\n' C1 C1 C1 C2 C2 C2 C3c C3c C3c C3f C3f C3f | shuf); do
  case $c in
    C1)  python3 tools/bt_sync.py --mac $MAC --rec-start; bluetoothctl disconnect $MAC; sleep 75   # page scan only
         python3 tools/bt_sync.py --mac $MAC --rec-stop; python3 tools/bt_sync.py --mac $MAC --dbg-rec 0 --dbg-log $L ;;
    C2)  python3 tools/bt_sync.py --mac $MAC --bench-load 75 --load-rec --dbg-every 0 --dbg-log $L ;;     # link, no data
    C3c) python3 tools/bt_sync.py --mac $MAC --bench-load 75 --load-rec --load-file LOAD.WAV --load-kbps 48 \
           --dbg-every 15 --dbg-log $L ;;
    C3f) python3 tools/bt_sync.py --mac $MAC --bench-load 75 --load-rec --load-file LOAD.WAV --load-kbps 0 \
           --burst-size 65536 --dbg-every 15 --dbg-log $L ;;
  esac
  echo "$c $(grep -o 'DBG,REC,0,seq=[0-9]*,file=[^,]*' $L | tail -n 1 | sed 's/.*file=//')" >> $D/plan.txt
  bluetoothctl disconnect $MAC; sleep 5
done
```
Cm block: only C2 x1. The session ends by itself at `secs` (Home). C3c's 48 KiB/s is the design's F1b starting cap;
C3f with 64 KiB windows is the worst case a fast client produces today.

**C. After the BT session, radio off: C0b x3, then C5 x3** (user, ~12 min; skip C5 without a paired headset):
C0b: on Home, REC on 75 s / off, three times; then cable in, the agent saves the ring (`sudo python3
tools/dev_cycle.py log > $D/ring_C0b.log`: headphones mode logs into the same 4 KiB ring), cable out. C5: Bluetooth
tile -> Headphones, wait until the headset is connected, back to Home (headphones stay on in the background), then
REC on 75 s / off three times (the stream is suspended while recording, the link stays). Then cable in (USB disk
mode stops headphones mode first).

**D. Fresh boot again: C0 x1** (agent then user, ~3 min):
```bash
sudo python3 tools/dev_cycle.py log > $D/ring_C.log
sudo python3 tools/openfw_usb.py --reboot normal
```
User: unplug, REC 75 s once (third C0 repeat: catches drift over the block), cable in.

**E. Pull and analyse** (agent):
```bash
sudo python3 tools/dev_cycle.py log > $D/ring_D.log
udisksctl mount -b /dev/sda1 && cp $M/record/REC_*.wav $M/BTBENCH.LOG $D/ && rm -f $M/record/REC_*.wav && sync && \
  udisksctl unmount -b /dev/sda1
grep -h '^BENCH rec' $D/ring_*.log | sed 's/.* file=\([^ ]*\) .*/\1/'   # REC switch captures, in time order
```
Label the files from the order above (A: C0_1 C0_2; C: C0b_1..3, C5_1..3; D: C0_3) and `plan.txt` (B), then:
```bash
B="--baseline C0_1=$D/REC_a.wav --baseline C0_2=$D/REC_b.wav --baseline C0_3=$D/REC_c.wav"
python3 tools/rf_audio_check.py $B --capture C0b_1=$D/REC_d.wav ... --capture C3f_3=$D/REC_x.wav \
  --bench-log $D/ring_A.log --bench-log $D/ring_C0b.log --bench-log $D/ring_C.log --bench-log $D/ring_D.log \
  --bench-log $D/dbg_B.log \
  --trim-start-s 7.5 --trim-end-s 7.5 --md $D/report.md --json $D/report.json
```
A capture whose `BENCH rec` line is missing (the ring wrapped) is analysed without the device counters (its row says
`(e) length ... not checked`); the audio metrics still apply. Every condition must pass at both gains and both channel modes (design decision 2); a single (e) finding fails its
condition. (f) blind ABX (user, headphones): C0 vs C3f of the gain 31 mono T block and of the listening block, 20
trials with any ABX tool on the laptop; pass at <= 12/20 correct.

**What the result selects** (design 2.6): all of C1, C2, C3c, C3f pass -> L0 (C3c only -> L1 at a lower cap); C1
passes but C2/C3 fail -> L2 (radio up, no ACL during REC); C1 fails -> L3 or no background sync (Q3). C0b failing
means the controller is not fully off after a session (a bug to fix before anything else). C5 is today's shipped
headphones behaviour: report it, it gates nothing here.

### M3 — CPU and timing margin with today's firmware (~1.5 h, mostly unattended)

Gain 20, mono, DSP 1 (the user's usual settings; audio is not analysed), cable may stay in (charging is fine here).
Agent:
```bash
D=$S/m3; mkdir -p $D
sudo python3 tools/openfw_usb.py --set-dsp 1 --set-gain 20 --set-ch 1 --set-vor 0   # user: unplug, wait for Home, plug
udisksctl mount -b /dev/sda1 && printf 'secs=3600 exit=auto\n' > $M/BTBENCH.RUN && sync && udisksctl unmount -b /dev/sda1
sudo python3 tools/openfw_usb.py --reboot normal; sleep 20
python3 tools/bt_sync.py --mac $MAC --bench-load 2400 --load-rec --load-file LOAD.WAV --load-kbps 48 --load-storm 30 \
  --dbg-every 10 --dbg-log $D/m3_48.log                                   # 40 min at the F1b starting cap + storm
for k in 24 64 96 0; do
  python3 tools/bt_sync.py --mac $MAC --bench-load 240 --load-rec --load-file LOAD.WAV --load-kbps $k --load-storm 30 \
    --burst-size 65536 --dbg-every 10 --dbg-log $D/m3_$k.log; sleep 5
done
```
(The session's `secs` caps the total at 60 min; the 3600 s maximum of `BTBENCH.RUN` is why the long run is 40 min.)
User: nothing; do not touch the device or the REC switch. Afterwards (cable in, USB disk mode comes back by itself):
```bash
sudo python3 tools/dev_cycle.py log > $D/ring.log
udisksctl mount -b /dev/sda1 && cp $M/BTBENCH.LOG $D/ && rm -f $M/record/REC_*.wav && sync && udisksctl unmount -b /dev/sda1
python3 - $D/m3_*.log <<'PY'
import re, sys
for p in sys.argv[1:]:
    rec = [l for l in open(p) if "DBG,REC,0," in l][-1]
    kv = dict(t.split("=", 1) for t in re.split(r"[ ,]", rec.strip()) if "=" in t)
    live = [dict(t.split("=", 1) for t in l.strip().split(",") if "=" in t) for l in open(p) if "DBG,LIVE," in l]
    idle = min((int(d["idle_pm"]) for d in live if d.get("idle_pm", "-") != "-"), default=-1)
    ok = (kv["ov"] == kv["lost"] == kv["acct"] == "0" and int(kv["gap_max_us"]) <= 96000
          and int(kv["idle_min_pm"] if kv["idle_min_pm"] != "-" else 0) >= 200)
    print(f"{p}: {'PASS' if ok else 'FAIL'} t_ms={kv['t_ms']} gets_b={kv['gets_b']} ov={kv['ov']} lost={kv['lost']} "
          f"acct={kv['acct']} drift={kv['drift']} gap_max_us={kv['gap_max_us']} wr_max_us={kv['wr_max_us']} "
          f"ckpt_max_us={kv['ckpt_max_us']} idle_min_pm={kv['idle_min_pm']} (DBG polls: {idle})")
PY
grep -c SDW_MDTO $D/ring.log $D/BTBENCH.LOG                             # must be 0
```
Pass per run (design M3): `ov` = `lost` = 0, `acct` = 0, `drift` within the C0 captures' range (+-1 block), max
recorder-loop gap `gap_max_us` <= 96000 (25 % of today's 384 ms ring), idle >= 20 % (`idle_min_pm` >= 200), no
`SDW_MDTO`. The highest passing cap, halved, is the F1b default for S3. Today's firmware has no recorder-first
scheduling (F1), so failing caps here are the S3 baseline, not a verdict on the design. Also report `BENCH tasks`
lines from `BTBENCH.LOG` (which task takes the CPU while GETS runs) and the `gets_b` / time actually reached per cap.

**Afterwards:** restore the user's settings (`sudo python3 tools/openfw_usb.py --set-dsp .. --set-gain .. --set-ch ..
--set-vor ..` from `$S/user_audio_cfg.txt`), delete `record/LOAD.WAV`, and leave the latest BT-branch build on the
device (a release `make -C open_fw BT=1` image: a bench image honours `SD:/BTBENCH.RUN`).
