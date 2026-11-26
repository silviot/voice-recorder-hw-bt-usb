# M2 / C1: does an idle radio change the recording? (2026-11-26, night)

Design: `docs/BT_SYNC_SPEED_AND_RECORDING.md` section 3 (M2). Tool: `tools/rf_audio_check.py --quiet-mode`.

## Setup

- Night, recorder lying on the desk, USB cable plugged in (bench control + log), nobody touching it.
- Recording profile for every run: gain 31, mono, DSP bypass (dsp=0), VOR off - set by the bench
  `BTBENCH.RUN` recording runs, restored afterwards (counter lines confirm `ch=1 gain=31 dsp=0 vor=0` for all six).
- Firmware: bench image built from `exp/overnight-s3-rec`, `0x3FBB`.
- C0 (radio off, `mode=rec`): C0_1 = REC_0032, C0_2 = REC_0034, C0_3 = REC_0036.
- C1 (radio on and idle, `mode=phone rec=1`, phone app stopped, no link): C1_1 = REC_0033, C1_2 = REC_0035,
  C1_3 = REC_0037. C0/C1 runs interleaved (C0_1, C1_1, C0_2, ...), 75 s each.
- Captures are named `<label>.wav` on the host; `labels.txt` maps label -> device file for the counter lines.

Command (in the capture folder):

```
python3 tools/rf_audio_check.py --signal silence --quiet-mode --labels labels.txt \
  --baseline C0_1=C0_1.wav --baseline C0_2=C0_2.wav --baseline C0_3=C0_3.wav \
  --capture C1_1=C1_1.wav --capture C1_2=C1_2.wav --capture C1_3=C1_3.wav \
  --bench-log btbench_C0_1.log ... --bench-log btbench_C1_3.log
```

## Result

- **C1_1 PASS, C1_2 PASS, C1_3 FAIL** (overall NOT PASS by the tool's bar). All three C0 baselines pass
  continuity, counters and burst classification.
- Night captures contain 1-7 s of low-frequency mechanical rumble (mostly 30-260 Hz, up to +58.7 dB over the quiet level of
  about -86.6 dBFS) in C0_1 and in all three C1 captures. Without `--quiet-mode` every whole-file metric is swamped
  (the first pass reported C0_1 at -41.6 dBFS against -86.4 for C0_2/C0_3). Quiet mode excluded 0-14 s per
  capture and kept 60-75 s; every excluded burst has an 800 Hz comb share <= 0.12 % and no tooth more than
  3.5 dB above its flanks (no RF burst hiding as a transient).
- In the quiet seconds the radio-idle captures match the radio-off baseline: noise floor within -0.42..+0.12 dB
  (A-weighted -0.32..+0.10 dB), no spur target on the 1600/800/400/266.7 Hz combs more than 0.73 dB above the
  baseline (C1_1, C1_2; C1_3 see below), no envelope line, fold depth at or below the baseline. Device counters: ov = lost = acct = 0, samples =
  wav_smp, block clock bdrift = -1 like every C0.
- **C1_3 FAIL** is one feature: spur target 2x40 Hz +4.5 dB and two "narrow peaks" at 78/79 Hz (+4.5/+5.6 dB,
  7 dB prominence). A per-second look shows a 76-82 Hz hump about 5 Hz wide that wanders, present only in the first
  ~20 s of C1_3 (8-15 dB over its neighbourhood) and gone afterwards. It is not on a BT timing frequency (not a
  sharp line, not the 7.8125 Hz DMA-block series, not the 800 Hz comb), not near C1_3's rumble burst (30-36 s), and
  absent from C1_1 and C1_2 under the same radio state. Most likely an acoustic/mechanical source in the room
  (a motor or fan); the tool's bar is left as is. Repeat C1 to settle it: a C1 capture without the hump passing
  would confirm the reading; a hump that recurs with the radio would not.

## Tool report

## RF audio check

Baseline: C0_1.wav, C0_2.wav, C0_3.wav

**Overall: NOT PASS**

### C0_1 - PASS

baseline (continuity, counters, quiet bursts): `C0_1.wav`, 74.88 s, 1 ch, ch0 silence floor -86.383 dBFS (-88.111 A); quiet mode: 60.53 s kept, 14.0 s excluded in 4 burst(s), quiet block level -86.78 dBFS

| metric | ch | value | baseline | delta | limit | verdict |
|---|---|---|---|---|---|---|
| (q) quiet seconds kept |  | 60.53 | 14.0 s excluded |  | >= 30 s (blocks > median +6 dB, +-1 guard) | **PASS** |
| (q) excluded bursts on BT comb |  | 0 | 0 |  | none: 800 Hz comb share < 0.05, no tooth >= 10 dB prom and > quiet +3 dB | **PASS** |
| (e) zero runs |  | 0 | 0 |  | none >= 2048 samples | **PASS** |
| (e) device record |  | REC_0032.wav | None |  | matched by REC_0032.WAV (explicit name) | **PASS** |
| (e) length vs device clock |  | 1198080 | 1199442 | -1362 | +-2048 | **PASS** |
| (e) samples = device wav_smp |  | 1198080 | 1198080 | 0 | exact | **PASS** |
| (e) device ov |  | 0 | 0 |  | 0 | **PASS** |
| (e) device lost |  | 0 | 0 |  | 0 | **PASS** |
| (e) device acct |  | 0 | 0 |  | 0 | **PASS** |
| (e) block-clock drift vs C0 |  | -1 | -1..-1 | 0.0 | +-8 smp outside the C0 range | **PASS** |
| (setup) gain/dsp/vor/ch |  | 31/0/0/1 | 31/0/0/1 |  | = baseline | **PASS** |

- (q) excluded bursts on BT comb: `[{"t_s": 10.25, "dur_s": 3.0, "peak_db_re_quiet": 57.6, "band_hz": "36-175", "peak_hz": 45.0, "share_800": 0.0, "share_1600": 0.0, "max_tooth": {"hz": 800.0, "prom_db": 3.3, "re_quiet_db": 36.2}, "bt_comb": false}, {"t_s": 33.25, "dur_s": 4.0, "peak_db_re_quiet": 58.7, "band_hz": "53-134", "peak_hz": 85.0, "share_800": 0.0, "share_1600": 0.0, "max_tooth": {"hz": 2400.0, "prom_db": 3.0, "re_quiet_db": 17.4}, "bt_comb": false}, {"t_s": 55.25, "dur_s": 4.0, "peak_db_re_quiet": 19.5, "band_hz": "55-1499", "peak_hz": 74.0, "share_800": 0.0007, "share_1600": 0.0002, "max_tooth": {"hz": 4000.0, "prom_db": 2.7, "re_quiet_db": 2.0}, "bt_comb": false}, {"t_s": 61.25, "dur_s": 3.0, "peak_db_re_quiet": 11.0, "band_hz": "83-5359", "peak_hz": 500.0, "share_800": 0.0012, "share_1600": 0.0001, "max_tooth": {"hz": 800.0, "prom_db": 1.9, "re_quiet_db": -1.6}, "bt_comb": false}]`

### C0_2 - PASS

baseline (continuity, counters, quiet bursts): `C0_2.wav`, 74.88 s, 1 ch, ch0 silence floor -86.459 dBFS (-87.793 A); quiet mode: 74.53 s kept, 0.0 s excluded in 0 burst(s), quiet block level -86.57 dBFS

| metric | ch | value | baseline | delta | limit | verdict |
|---|---|---|---|---|---|---|
| (q) quiet seconds kept |  | 74.53 | 0.0 s excluded |  | >= 30 s (blocks > median +6 dB, +-1 guard) | **PASS** |
| (q) excluded bursts on BT comb |  | 0 | 0 |  | none: 800 Hz comb share < 0.05, no tooth >= 10 dB prom and > quiet +3 dB | **PASS** |
| (e) zero runs |  | 0 | 0 |  | none >= 2048 samples | **PASS** |
| (e) device record |  | REC_0034.wav | None |  | matched by REC_0034.WAV (explicit name) | **PASS** |
| (e) length vs device clock |  | 1198080 | 1199570 | -1490 | +-2048 | **PASS** |
| (e) samples = device wav_smp |  | 1198080 | 1198080 | 0 | exact | **PASS** |
| (e) device ov |  | 0 | 0 |  | 0 | **PASS** |
| (e) device lost |  | 0 | 0 |  | 0 | **PASS** |
| (e) device acct |  | 0 | 0 |  | 0 | **PASS** |
| (e) block-clock drift vs C0 |  | -1 | -1..-1 | 0.0 | +-8 smp outside the C0 range | **PASS** |
| (setup) gain/dsp/vor/ch |  | 31/0/0/1 | 31/0/0/1 |  | = baseline | **PASS** |

### C0_3 - PASS

baseline (continuity, counters, quiet bursts): `C0_3.wav`, 74.88 s, 1 ch, ch0 silence floor -86.441 dBFS (-87.848 A); quiet mode: 74.53 s kept, 0.0 s excluded in 0 burst(s), quiet block level -86.56 dBFS

| metric | ch | value | baseline | delta | limit | verdict |
|---|---|---|---|---|---|---|
| (q) quiet seconds kept |  | 74.53 | 0.0 s excluded |  | >= 30 s (blocks > median +6 dB, +-1 guard) | **PASS** |
| (q) excluded bursts on BT comb |  | 0 | 0 |  | none: 800 Hz comb share < 0.05, no tooth >= 10 dB prom and > quiet +3 dB | **PASS** |
| (e) zero runs |  | 0 | 0 |  | none >= 2048 samples | **PASS** |
| (e) device record |  | REC_0036.wav | None |  | matched by REC_0036.WAV (explicit name) | **PASS** |
| (e) length vs device clock |  | 1198080 | 1199461 | -1381 | +-2048 | **PASS** |
| (e) samples = device wav_smp |  | 1198080 | 1198080 | 0 | exact | **PASS** |
| (e) device ov |  | 0 | 0 |  | 0 | **PASS** |
| (e) device lost |  | 0 | 0 |  | 0 | **PASS** |
| (e) device acct |  | 0 | 0 |  | 0 | **PASS** |
| (e) block-clock drift vs C0 |  | -1 | -1..-1 | 0.0 | +-8 smp outside the C0 range | **PASS** |
| (setup) gain/dsp/vor/ch |  | 31/0/0/1 | 31/0/0/1 |  | = baseline | **PASS** |

### C1_1 - PASS

capture: `C1_1.wav`, 74.88 s, 1 ch, ch0 silence floor -86.851 dBFS (-88.235 A); quiet mode: 65.53 s kept, 9.0 s excluded in 2 burst(s), quiet block level -86.88 dBFS

| metric | ch | value | baseline | delta | limit | verdict |
|---|---|---|---|---|---|---|
| (b) baseline repeatability | 0 | 0.28 | None |  | median level offset spread <= 1.0 dB | **PASS** |
| (a) noise floor | 0 | -86.85 | -86.43 | -0.42 | +-0.5 dB (spread 0.08) | **PASS** |
| (a) noise floor A-wtd | 0 | -88.23 | -87.92 | -0.32 | +-0.5 dB (spread 0.32) | **PASS** |
| (b) spur targets | 0 | worst 5x800 Hz | C0 mean+sd | 0.68 | <= +3.0 dB | **PASS** |
| (b) new narrow peaks | 0 | 0 | 0 |  | none > +3.0 dB with >= 6.0 dB prominence | **PASS** |
| (c) envelope 0.78125 Hz | 0 | -47.64 | -39.92 | -7.72 | line: <= +3.0 dB or prom < 6.0 dB (prom 1.3) | **PASS** |
| (c) envelope 2.34375 Hz | 0 | -51.33 | -39.83 | -11.5 | line: <= +3.0 dB or prom < 6.0 dB (prom -2.4) | **PASS** |
| (c) envelope 7.5 Hz | 0 | -48.28 | -41.42 | -6.86 | line: <= +3.0 dB or prom < 6.0 dB (prom 0.6) | **PASS** |
| (c) envelope lines | 0 | 0 | 0 |  | none 0.2-40 Hz > +3.0 dB with >= 6.0 dB prom | **PASS** |
| (c) 1.28 s fold depth | 0 | -32.98 | -31.36 | -1.62 | <= +3.0 dB | **PASS** |
| (q) quiet seconds kept |  | 65.53 | 9.0 s excluded |  | >= 30 s (blocks > median +6 dB, +-1 guard) | **PASS** |
| (q) excluded bursts on BT comb |  | 0 | 0 |  | none: 800 Hz comb share < 0.05, no tooth >= 10 dB prom and > quiet +3 dB | **PASS** |
| (e) zero runs |  | 0 | 0 |  | none >= 2048 samples | **PASS** |
| (e) device record |  | REC_0033.wav | None |  | matched by REC_0033.WAV (explicit name) | **PASS** |
| (e) length vs device clock |  | 1198080 | 1199170 | -1090 | +-2048 | **PASS** |
| (e) samples = device wav_smp |  | 1198080 | 1198080 | 0 | exact | **PASS** |
| (e) device ov |  | 0 | 0 |  | 0 | **PASS** |
| (e) device lost |  | 0 | 0 |  | 0 | **PASS** |
| (e) device acct |  | 0 | 0 |  | 0 | **PASS** |
| (e) block-clock drift vs C0 |  | -1 | -1..-1 | 0.0 | +-8 smp outside the C0 range | **PASS** |
| (setup) gain/dsp/vor/ch |  | 31/0/0/1 | 31/0/0/1 |  | = baseline | **PASS** |

- (q) excluded bursts on BT comb: `[{"t_s": 0.25, "dur_s": 2.0, "peak_db_re_quiet": 55.0, "band_hz": "30-129", "peak_hz": 42.0, "share_800": 0.0, "share_1600": 0.0, "max_tooth": {"hz": 4800.0, "prom_db": 2.4, "re_quiet_db": 10.8}, "bt_comb": false}, {"t_s": 3.25, "dur_s": 7.0, "peak_db_re_quiet": 58.3, "band_hz": "38-130", "peak_hz": 65.0, "share_800": 0.0, "share_1600": 0.0, "max_tooth": {"hz": 5600.0, "prom_db": 1.3, "re_quiet_db": 4.5}, "bt_comb": false}]`

### C1_2 - PASS

capture: `C1_2.wav`, 74.88 s, 1 ch, ch0 silence floor -86.563 dBFS (-87.819 A); quiet mode: 63.0 s kept, 11.53 s excluded in 3 burst(s), quiet block level -86.59 dBFS

| metric | ch | value | baseline | delta | limit | verdict |
|---|---|---|---|---|---|---|
| (b) baseline repeatability | 0 | 0.28 | None |  | median level offset spread <= 1.0 dB | **PASS** |
| (a) noise floor | 0 | -86.56 | -86.43 | -0.14 | +-0.5 dB (spread 0.08) | **PASS** |
| (a) noise floor A-wtd | 0 | -87.82 | -87.92 | 0.1 | +-0.5 dB (spread 0.32) | **PASS** |
| (b) spur targets | 0 | worst 9x400 Hz | C0 mean+sd | 0.73 | <= +3.0 dB | **PASS** |
| (b) new narrow peaks | 0 | 0 | 0 |  | none > +3.0 dB with >= 6.0 dB prominence | **PASS** |
| (c) envelope 0.78125 Hz | 0 | -44.92 | -39.92 | -5.0 | line: <= +3.0 dB or prom < 6.0 dB (prom 3.6) | **PASS** |
| (c) envelope 2.34375 Hz | 0 | -46.98 | -39.83 | -7.15 | line: <= +3.0 dB or prom < 6.0 dB (prom 1.6) | **PASS** |
| (c) envelope 7.5 Hz | 0 | -46.06 | -41.42 | -4.64 | line: <= +3.0 dB or prom < 6.0 dB (prom 2.5) | **PASS** |
| (c) envelope lines | 0 | 0 | 0 |  | none 0.2-40 Hz > +3.0 dB with >= 6.0 dB prom | **PASS** |
| (c) 1.28 s fold depth | 0 | -34.48 | -31.36 | -3.12 | <= +3.0 dB | **PASS** |
| (q) quiet seconds kept |  | 63.0 | 11.53 s excluded |  | >= 30 s (blocks > median +6 dB, +-1 guard) | **PASS** |
| (q) excluded bursts on BT comb |  | 0 | 0 |  | none: 800 Hz comb share < 0.05, no tooth >= 10 dB prom and > quiet +3 dB | **PASS** |
| (e) zero runs |  | 0 | 0 |  | none >= 2048 samples | **PASS** |
| (e) device record |  | REC_0035.wav | None |  | matched by REC_0035.WAV (explicit name) | **PASS** |
| (e) length vs device clock |  | 1198080 | 1199150 | -1070 | +-2048 | **PASS** |
| (e) samples = device wav_smp |  | 1198080 | 1198080 | 0 | exact | **PASS** |
| (e) device ov |  | 0 | 0 |  | 0 | **PASS** |
| (e) device lost |  | 0 | 0 |  | 0 | **PASS** |
| (e) device acct |  | 0 | 0 |  | 0 | **PASS** |
| (e) block-clock drift vs C0 |  | -1 | -1..-1 | 0.0 | +-8 smp outside the C0 range | **PASS** |
| (setup) gain/dsp/vor/ch |  | 31/0/0/1 | 31/0/0/1 |  | = baseline | **PASS** |

- (q) excluded bursts on BT comb: `[{"t_s": 18.25, "dur_s": 3.0, "peak_db_re_quiet": 53.3, "band_hz": "73-259", "peak_hz": 135.0, "share_800": 0.0001, "share_1600": 0.0, "max_tooth": {"hz": 5600.0, "prom_db": 1.4, "re_quiet_db": 7.1}, "bt_comb": false}, {"t_s": 36.25, "dur_s": 7.0, "peak_db_re_quiet": 56.0, "band_hz": "39-147", "peak_hz": 70.0, "share_800": 0.0, "share_1600": 0.0, "max_tooth": {"hz": 1600.0, "prom_db": 1.6, "re_quiet_db": 21.3}, "bt_comb": false}, {"t_s": 73.25, "dur_s": 1.53, "peak_db_re_quiet": 57.0, "band_hz": "65-1609", "peak_hz": 500.0, "share_800": 0.0006, "share_1600": 0.0002, "max_tooth": {"hz": 4000.0, "prom_db": 3.5, "re_quiet_db": -2.7}, "bt_comb": false}]`

### C1_3 - FAIL

capture: `C1_3.wav`, 74.88 s, 1 ch, ch0 silence floor -86.312 dBFS (-87.877 A); quiet mode: 68.53 s kept, 6.0 s excluded in 1 burst(s), quiet block level -86.47 dBFS

| metric | ch | value | baseline | delta | limit | verdict |
|---|---|---|---|---|---|---|
| (b) baseline repeatability | 0 | 0.28 | None |  | median level offset spread <= 1.0 dB | **PASS** |
| (a) noise floor | 0 | -86.31 | -86.43 | 0.12 | +-0.5 dB (spread 0.08) | **PASS** |
| (a) noise floor A-wtd | 0 | -87.88 | -87.92 | 0.04 | +-0.5 dB (spread 0.32) | **PASS** |
| (b) spur targets | 0 | worst 2x40 Hz | C0 mean+sd | 4.49 | <= +3.0 dB | **FAIL** |
| (b) new narrow peaks | 0 | 2 | 0 |  | none > +3.0 dB with >= 6.0 dB prominence | **FAIL** |
| (c) envelope 0.78125 Hz | 0 | -43.21 | -39.92 | -3.29 | line: <= +3.0 dB or prom < 6.0 dB (prom 4.0) | **PASS** |
| (c) envelope 2.34375 Hz | 0 | -47.62 | -39.83 | -7.79 | line: <= +3.0 dB or prom < 6.0 dB (prom -0.4) | **PASS** |
| (c) envelope 7.5 Hz | 0 | -46.83 | -41.42 | -5.41 | line: <= +3.0 dB or prom < 6.0 dB (prom 0.4) | **PASS** |
| (c) envelope lines | 0 | 0 | 0 |  | none 0.2-40 Hz > +3.0 dB with >= 6.0 dB prom | **PASS** |
| (c) 1.28 s fold depth | 0 | -34.23 | -31.36 | -2.87 | <= +3.0 dB | **PASS** |
| (q) quiet seconds kept |  | 68.53 | 6.0 s excluded |  | >= 30 s (blocks > median +6 dB, +-1 guard) | **PASS** |
| (q) excluded bursts on BT comb |  | 0 | 0 |  | none: 800 Hz comb share < 0.05, no tooth >= 10 dB prom and > quiet +3 dB | **PASS** |
| (e) zero runs |  | 0 | 0 |  | none >= 2048 samples | **PASS** |
| (e) device record |  | REC_0037.wav | None |  | matched by REC_0037.WAV (explicit name) | **PASS** |
| (e) length vs device clock |  | 1198080 | 1199152 | -1072 | +-2048 | **PASS** |
| (e) samples = device wav_smp |  | 1198080 | 1198080 | 0 | exact | **PASS** |
| (e) device ov |  | 0 | 0 |  | 0 | **PASS** |
| (e) device lost |  | 0 | 0 |  | 0 | **PASS** |
| (e) device acct |  | 0 | 0 |  | 0 | **PASS** |
| (e) block-clock drift vs C0 |  | -1 | -1..-1 | 0.0 | +-8 smp outside the C0 range | **PASS** |
| (setup) gain/dsp/vor/ch |  | 31/0/0/1 | 31/0/0/1 |  | = baseline | **PASS** |

- (b) spur targets: `["2x40 Hz +4.5 dB"]`

- (b) new narrow peaks: `[{"hz": 78.0, "excess_db": 4.49, "prom_db": 7.27}, {"hz": 79.0, "excess_db": 5.64, "prom_db": 7.08}]`

- (q) excluded bursts on BT comb: `[{"t_s": 30.25, "dur_s": 6.0, "peak_db_re_quiet": 53.2, "band_hz": "60-182", "peak_hz": 98.0, "share_800": 0.0, "share_1600": 0.0, "max_tooth": {"hz": 4000.0, "prom_db": 1.8, "re_quiet_db": 13.0}, "bt_comb": false}]`
