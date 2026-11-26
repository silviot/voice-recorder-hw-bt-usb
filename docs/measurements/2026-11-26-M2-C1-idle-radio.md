# M2 / C1: does an idle radio change the recording? (2026-11-26, night)

Design: `docs/BT_SYNC_SPEED_AND_RECORDING.md` section 3 (M2). Tool: `tools/rf_audio_check.py --band-split`
(fail-closed by construction; it replaces the exclusion-based `--quiet-mode`, which is now an alias of it).

## Setup

- Night, recorder lying on the desk, USB cable plugged in (bench control + log), nobody touching it.
- Recording profile for every run: gain 31, mono, DSP bypass (dsp=0), VOR off - set by the bench
  `BTBENCH.RUN` recording runs, restored afterwards (counter lines confirm `ch=1 gain=31 dsp=0 vor=0` for all).
- Firmware: bench image built from `exp/overnight-s3-rec`, `0x3FBB`.
- C0 (radio off, `mode=rec`): C0_1 = REC_0032, C0_2 = REC_0034, C0_3 = REC_0036 (75 s); later C0_b1 = REC_0039,
  C0_b2 = REC_0041 (90 s), interleaved with the C3p runs.
- C1 (radio on and idle, `mode=phone rec=1`, phone app stopped, no link): C1_1 = REC_0033, C1_2 = REC_0035,
  C1_3 = REC_0037. C0/C1 runs interleaved (C0_1, C1_1, C0_2, ...), 75 s each.
- C3p (phone mode, 90 s recording requested, "sync now" pressed on the phone): C3p_1 = REC_0038, C3p_2 = REC_0040,
  C3p_3 = REC_0042. All three recordings ended early on an SD write error (71.6 / 31.2 / 32.0 s recorded).
- Captures are named `<label>.wav` on the host; `labels.txt` maps label -> device file for the counter lines.

Commands (in the capture folder `/home/silvio/dev/.build-scratch/night/caps`; reports there as `<run>.md` / `.json`):

```
# c1_bandsplit: C1 against C0_1..3
python3 tools/rf_audio_check.py --signal silence --band-split --labels labels.txt \
  --baseline C0_1=C0_1.wav --baseline C0_2=C0_2.wav --baseline C0_3=C0_3.wav \
  --capture C1_1=C1_1.wav --capture C1_2=C1_2.wav --capture C1_3=C1_3.wav \
  --bench-log C0_1=btbench_C0_1.log ... --bench-log C1_3=btbench_C1_3.log
# c13_bandsplit: + C0_b1, C0_b2 as baselines, + C3p_1..3 as captures (all eleven per-capture logs)
# quietbase_bandsplit: baselines C0_2, C0_3, C0_b1, C0_b2 only (no rumble); captures C0_1, C1_*, C3p_*
# loo_<C0>: each rumble-free C0 against the other three (leave one out)
```

## Result

**No capture passes; these night captures cannot answer M2 / C1.** The radio is neither convicted nor cleared:
every high-band failure of C1_1..3 and C3p_1 lies in the seconds of a mechanical rumble event. Under the band-split
bar (below) captures with rumble events can only FAIL or be INCONCLUSIVE.

| run | baselines | C1_1 | C1_2 | C1_3 | C3p_1 | C3p_2 | C3p_3 |
|---|---|---|---|---|---|---|---|
| c1_bandsplit | C0_1..3 | FAIL / REVIEW | FAIL / REVIEW | FAIL / REVIEW | - | - | - |
| c13_bandsplit | C0_1..3, C0_b1, C0_b2 | FAIL / PASS | FAIL / PASS | FAIL / PASS | FAIL / PASS | FAIL (continuity; high band INCONCLUSIVE) / REVIEW | FAIL (continuity; high band INCONCLUSIVE) / REVIEW |
| quietbase_bandsplit | C0_2, C0_3, C0_b1, C0_b2 | FAIL / REVIEW | FAIL / REVIEW | FAIL / REVIEW | FAIL / REVIEW | FAIL (continuity, (c) envelope lines) / REVIEW | FAIL (continuity, (c) envelope lines) / REVIEW |

Each cell: the capture's verdict / the low band's ("PASS" there = no low-band FAIL outside rumble within the
baseline's). Every baseline passes its own continuity and counter checks in every run.

- **Real rumble is not confined below 300 Hz.** The rumble events (C0_1 at 11, 34-35, 57, 62 s; C1_1 at 3-9 s;
  C1_2 at 19-20, 37-41 s; C1_3 at 31-35 s; C3p_1 at 15-22 s) are knocks: per second the 250-500 Hz octave reaches
  -43..-48 dBFS, 500 Hz - 1 kHz -51..-61, 1-2 kHz -58..-71, 2-4 kHz -62..-80, 4-8 kHz -74..-87 dBFS, against a
  quiet octave level of about -95 dBFS; 1 ms high-band frames stand 60-66 dB over the median. The report's
  information line puts 100 % of the high band's excess energy of every rumble capture (C0_1, C1_1..3, C3p_1) in
  its low-band burst seconds.
- Hence C0_1 (rumble) and C0_2 / C0_3 (none) disagree in the high band (repeatability 3.8 dB, floors 10-25 dB
  apart): against C0_1..3 the high-band floors of C1 are INCONCLUSIVE, and what FAILS is where the C1 rumble events
  are louder than C0_1's: loudest second 2.4-8 kHz +4..+9 dB, octave floor 2.4-4.8 kHz +7.6..+11 dB, spur targets
  +8.5..+12.7 dB (broadband knocks covering the target bins), envelope lines at 0.3-10 Hz, C1_2 15 clicks against at
  most 10. Against the rumble-free C0s every rumble capture fails by 20-35 dB.
- Low band of C1 against C0_1..3: REVIEW. Without the burst seconds C1_1 and C1_2 are 0.6 dB *quieter* below
  300 Hz than the baseline (the floor bar is two-sided); C1_3 carries the 76-82 Hz hump of its first ~20 s reported
  earlier ((L) spur target 2x40 Hz +5 dB, 3 narrow peaks, 40-80 Hz octave +1.8 dB).
- **C3p_1..3 fail continuity, as they must**: length against the device clock -2114 / -2164 / -2278 samples
  (tolerance 2048), `acct = 2048` on the device, and each WAV holds 234 samples more than the counter's `wav_smp` -
  the SD write error that ended them. The recorded part is analysed all the same.
- **C3p_2 and C3p_3** (no rumble, radio active with a sync) pass every high-band floor, octave, loudest-second,
  spur, peak and click row against the rumble-free C0s and fail only "(c) envelope lines": C3p_2 at 15.5 / 32.0 /
  35.7 Hz (+3.1..6.7 dB over the baseline, prominence 6.7-8.4 dB), C3p_3 at 22.9 Hz (+7.2 dB, prominence 12.5 dB)
  and 39.1 Hz. C3p_3 also carries a 22-24 Hz line with harmonics at 45-47, 69, 114, 161 Hz in the low band (+20 dB
  over the baseline) and the same 22.9 Hz line in its low-band envelope. These files are 31-32 s long (2-3 envelope
  segments against 5-6 for the baselines), so their envelope rows are weaker evidence than a 75 s capture's; the
  22.9 Hz rhythm in both bands of C3p_3 is a lead for M3 (sync while recording), not a result.
- Leave one out among the rumble-free C0s: C0_3 and C0_b1 PASS; C0_2 and C0_b2 are REVIEW with the high band PASS
  (C0_b2: 20-300 Hz floor +3.8 dB over the others, +9 dB at 20-40 Hz; C0_2: one low-band envelope line). The night
  low band varies between radio-off repeats by more than the 0.5 dB floor bar: it is a REVIEW region by design.

What it takes to answer C1: captures without rumble events (a quieter hour or a decoupled support - the knocks come
through the desk), or enough repeats that the baselines' rumble statistics cover the captures'. Rumble-free C1
captures against C0_2, C0_3, C0_b1, C0_b2 would be judged on the full bar: those C0s pass each other's high band.

## Band split: fail-closed by construction (replaces `--quiet-mode`, same day)

History. The first `--quiet-mode` left out loud 1 s blocks; a review injected faults that it passed as
"transients". The fail-closed version (b460fe8, 0a1eedb) allowed only rumble-shaped bursts to be left out, but
re-reviews kept finding false passes: the guard seconds around a burst hid tones and BT-like trains; 250-500 Hz was
unchecked; a 330 Hz tone or 300-480 Hz noise next to rumble, and 11.25 ms trains of 330 + 450 Hz bursts, passed.
Any scheme that leaves seconds out has seconds to hide in. It is withdrawn; `--quiet-mode` is now an alias of
`--band-split`, and the earlier quiet-mode verdicts in this report (C1_1 PASS, C1_2 PASS, C1_3 FAIL) are void.

`--band-split` (`tools/rf_audio_check.py`, (s) in its header) leaves NO second out of any verdict:

- **Split**: a linear-phase FIR high-pass (Kaiser, 3419 taps; stop edge 300 Hz at -100 dB, pass edge 330 Hz,
  ripple 0.0001 dB) over the whole file, identical for baselines and captures; the low band is the signal minus the
  high band. Rumble below 300 Hz cannot reach the high band; whatever a rumble event has above 300 Hz is judged
  there like anything else.
- **High band (>= 300 Hz)**, whole analysis window, decides FAIL / INCONCLUSIVE together with continuity: (a) floor
  and A-weighted floor 300 Hz - 8 kHz, each octave from 300 Hz (one-sided: a rise above +0.5 dB inside the
  baselines' spread is INCONCLUSIVE, beyond it FAIL), and the loudest 1 s of the band and of each octave against
  the baselines' loudest + 1 dB (a short event is not diluted by the average); (b) spur targets >= 300 Hz
  (k*1600 / 800 / 400 / 266.67 Hz, and now k*216.67 Hz, a 4.615 ms TDMA frame) and narrow peaks; (c) envelope of
  the high-passed signal (targets, lines, 1.28 s fold) and clicks: 1 ms frames 20 dB over the window's median, or a
  filter matched to a DC step's high-band shape 20 dB over its robust RMS (a 0.002 FS step: about 35 dB), counted
  against the baselines scaled to length - up to their most PASS, above it INCONCLUSIVE, more than 2 above FAIL,
  periodic FAIL, one event 10 dB louder than any baseline event FAIL; (d) tones >= 300 Hz. A baseline that fails
  its own repeatability (rumble in some repeats only) turns an excess over its mean + sd into INCONCLUSIVE unless
  the excess also clears the loudest repeat (then FAIL). Never PASS.
- **Low band (< 300 Hz)**, rows `(L) ...`, the same metrics on the whole window, reported separately as the
  "mechanical rumble region". They never FAIL a capture: a low-band row that does not pass makes the capture REVIEW
  unless rumble within the baseline's explains it - low-band bursts (1 s blocks > median + 6 dB, +-1 block) no more
  and no longer in total than in the baseline with the most, no burst rhythm (interval CV >= 0.2), no burst
  periodic at one DMA block (128 ms autocorrelation), and with the burst seconds left out (in the baselines too) no
  (L) row FAILS. The explanation never touches the high band.
- **Continuity (e)** unchanged, whole file; tone phase steps on the high band for tones >= 300 Hz.
- Verdicts: FAIL / INCONCLUSIVE (high band, continuity) before REVIEW (low band) before PASS; exit status 0 only
  when every capture passes. Information only: the share of the high band's excess energy in low-band burst seconds.

Self-test (`python3 tools/test_tools.py`; 86 checks): synthetic rumble in baselines and capture PASSES (low band
explained), 2 thumps within the baseline's PASS; 7 thumps, periodic thumps, a 128 ms periodic low-band burst and a
steady 120 Hz line -> REVIEW; clicks, broadband and 1-6 kHz hiss, sparse +30 dB bursts, combs on k*266.67 (no
800 Hz teeth), k*400 and k*800 Hz, comb lines under rumble, page-scan cadence, a DC step, single-sample spikes, a
+1 dB floor, a 330 Hz tone next to rumble and in quiet, 300-480 Hz noise next to rumble, 11.25 ms 330 + 450 Hz
bursts every 1.28 s, and 1.25 ms bursts every 11.25 ms next to rumble -> FAIL; a baseline with a knock in one repeat
-> INCONCLUSIVE, not PASS; 997 Hz tone captures with rumble PASS, +3 dB noise and a 1-sample loss FAIL; the FIR's
stop band and ripple.

Injections on real captures (scratch script; levels relative to the host's high-band quiet level, -87.7 dBFS):
host C0_3 against C0_2, C0_b1, C0_b2 (clean host PASS), and the same host with C1_1's rumble (its part below
200-280 Hz) pasted at 20-30 s (clean host: high band PASS, low band REVIEW - that rumble is not in these baselines):

| injection | C0_3 host | C0_3 + rumble host |
|---|---|---|
| none | PASS | REVIEW (low band) |
| floor +1.5 dB | FAIL | FAIL |
| page scan 4.5 ms every 1.28 s, +10 / +20 / +30 dB | FAIL | FAIL |
| 4.5 ms +30 dB every 2.56 / 5.12 / 10.24 s | FAIL | FAIL |
| 4 x 1 s broadband hiss +12 dB; 2 x 1 s 1-6 kHz hiss +10 dB | FAIL | FAIL |
| 5 clicks 0.2 FS; 3 spikes 0.002 / 0.02 FS; 3 x 1 ms 0.01 FS pulses | FAIL | FAIL |
| combs k*400, k*800 (-5 dB), k*266.67 Hz (no 800 Hz teeth), 2 x 1-1.5 s | FAIL | FAIL |
| DC pulse 2 s, 0.002 / 0.01 / 0.05 FS | FAIL | FAIL |
| 330 Hz 1 s at the quiet level and 10 dB under it, next to the rumble and in quiet | FAIL | FAIL |
| 300-480 Hz noise 1 s next to the rumble, +10 / +0 dB | FAIL | FAIL |
| 11.25 ms 330 + 450 Hz bursts every 1.28 s, +10 / +0 dB | FAIL | FAIL |
| 1.25 ms 330 + 450 Hz bursts every 11.25 ms for 1 s next to the rumble, +10 dB | FAIL | FAIL |
| the same at +0 dB | **PASS** | REVIEW (low band only) |
| 2 / 6 / periodic low-frequency thumps (10-80 Hz) | REVIEW (low band) | REVIEW |

On the reviewer's host (C1_1 against C0_1..3) every case, the uninjected host included, FAILS. The one pass is a
sensitivity limit, not a place to hide: one second at 11 % duty at the floor's own level adds 0.03 dB to the
300-600 Hz floor and lifts that octave's loudest second by about 1 dB, inside the 0.9 dB by which radio-off repeats
differ (1/3-octave bands give about 1 dB as well). The same train 10 dB stronger, longer or periodic FAILS.
