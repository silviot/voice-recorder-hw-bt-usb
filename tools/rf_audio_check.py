#!/usr/bin/env python3
"""
rf_audio_check.py - "does the radio change the recording?" (docs/BT_SYNC_SPEED_AND_RECORDING.md section 3, M2).

Compares WAV captures from the recorder (16 kHz, 16-bit PCM, mono or stereo) against a baseline (C0: radio never on,
same gain / DSP profile / channels / signal) and prints a pass/fail table per capture, as Markdown and JSON.

Metrics (thresholds are the design's M2 bar, each configurable). The baseline reference is the mean of the
repeats plus one standard deviation where a per-repeat maximum would grow with the number of repeats. Verdicts are
PASS, FAIL or INCONCLUSIVE (the baseline cannot support the bar, or the setup differs); only PASS passes.
  (a) noise floor 20 Hz - 8 kHz, unweighted and A-weighted (dBFS, RMS of full scale = 0 dB; silence captures)
      PASS: within baseline mean +- 0.5 dB; INCONCLUSIVE when the repeats themselves spread by more than 0.5 dB
  (b) Welch PSD, 1 Hz bins (Blackman-Harris, 1 s segments, 50 % overlap), middle window (--trim-*):
      repeatability - the repeats' median level offset across the band: INCONCLUSIVE above 1 dB (or < 2 repeats)
      spurs   - the strongest bin within +-2 Hz of every target k*1600, k*800, k*400, k*266.7 (< 8 kHz) and k*40 Hz
                (k <= 5): PASS if no target exceeds the baseline's same window by > 3 dB
      peaks   - any 1 Hz bin 20 Hz - 7.99 kHz that exceeds the baseline by > 3 dB AND stands >= 6 dB above
                its own +-50 Hz median (a narrow line, not noise statistics): PASS if there is none
      (bins within +-6 Hz of the test tone's harmonics are excluded in tone captures)
  (c) envelope modulation: RMS envelope (x^2 low-passed to 45 Hz, 100 Hz rate) -> Welch (12.8 s segments): the targets 0.78, 2.34 and 7.5 Hz (page
      scan 1.28 s and its harmonics, 7.5 Hz) and any line 0.2 - 40 Hz (a burst rhythm the targets miss, e.g. a GETS
      burst every 78 ms = 12.8 Hz): FAIL if a bin exceeds the baseline by > 3 dB AND stands >= 6 dB above the
      envelope spectrum's local median (a line); fold: the envelope averaged over 1.28 s periods, depth = std /
      mean, FAIL if > max(baseline, the same envelope folded at non-1.28 s periods) + 3 dB
  (d) tone captures, 1 s blocks, each fitted with its own frequency (4-parameter sine fit: a tone that wanders by
      0.01 Hz must not read as noise): SNR and THD+N not worse than the baseline by more than 0.5 dB, the residual
      noise (dBFS, harmonics excluded) not above it by more than 0.5 dB; tone level within 0.3 dB of the baseline,
      else INCONCLUSIVE (SNR shifts 1:1 with level)
  (e) continuity (absolute; any finding fails the capture, whatever the other metrics show), over the whole file
      except --lim-cont-edge-s at each end (the REC switch click):
      zero runs >= 2048 samples (the recorder's overrun fill), tone phase steps > 0.05 rad (16 ms blocks, also split
      across two blocks; a sample-exact loss of 2048 samples is invisible at exactly 1000 Hz: use 997 Hz; losses of
      1-3 ms are near-invisible at any single frequency), length = 16000 * T +- 2048 (T from the device counters,
      --bench-log, or --expect-seconds), and with --bench-log the device counters: ov = 0, lost = 0, acct = 0, the
      file's samples = wav_smp, and the block clock (bdrift: DMA blocks vs the ISR's us_timer stamps, which resolves
      a few samples) within the C0 captures' drift range +- 8 samples - the detector for silent sub-block loss.
      With --bench-log a capture without a counter line FAILS (ring wrapped, renamed file) unless waived with
      --no-device-counters LABEL. Settings (gain, dsp, vor, ch) from the counters must match the baseline's.
      A capture is matched to its counter line by device file name: LABEL=path@REC_0032.wav, then --labels FILE
      (lines "LABEL REC_0032.wav", e.g. the bench runner's labels.txt), then the WAV's own name; a log given as
      --bench-log LABEL=log serves that capture first, and when it holds a single finished record that one is used.
  (s) --band-split (alias --quiet-mode): night captures carry seconds of mechanical rumble (40-150 Hz, near full
      scale) that swamp every whole-file metric. The file is split at --lim-split-hz (300 Hz) by a linear-phase FIR
      high-pass (Kaiser; stop edge 300 Hz at --lim-split-att-db 100 dB, pass edge --lim-split-pass-hz 330 Hz: nothing
      below 300 Hz reaches the high band; the low band is the signal minus the high band), applied identically to
      the baselines and the captures. NO SECOND IS LEFT OUT of
      any verdict: fail-closed by construction, there is no exclusion a BT artefact could hide in.
        HIGH BAND (>= 300 Hz; Bluetooth artefacts - slot / TDMA combs at k*1600 / 800 / 400 / 266.67 / 216.67 Hz,
          clicks, hiss, supply steps - all carry energy here): (a) floor and A-weighted floor 300 Hz - 8 kHz plus
          each octave from 300 Hz, (b) spurs (targets >= 300 Hz) and narrow peaks, (c) envelope of the high-passed
          signal and (c) clicks (1 ms frames of the high band, or a filter matched to a DC step's high-band shape,
          --lim-click-db over the window's level; counted against the baselines scaled to length: up to their most
          PASS, above it INCONCLUSIVE, above it by > --lim-click-margin FAIL, periodic FAIL, one event
          --lim-click-loud-db above the baselines' loudest FAIL; baselines that disagree by > the margin: above
          their fewest INCONCLUSIVE),
          (d) tones >= 300 Hz - on the whole analysis window; (a) also the loudest 1 s (0.5 s hop) of the band and
          of each octave against the baselines' loudest + --lim-second-db (1 dB), so a short event is not diluted
          by the whole-window average (a noisy baseline is caught by its repeatability row). Octave floors are one-sided (a rise above +0.5 dB
          within the baselines' spread INCONCLUSIVE, beyond it FAIL). A baseline that fails its own repeatability
          makes an excess over its mean + sd INCONCLUSIVE unless it also clears the loudest repeat (then FAIL). Rumble that leaks above 300 Hz (real rumble events
          carry knocks and rattles up to 8 kHz) is in the baselines too and is judged by the usual baseline
          statistics: when it makes the baselines disagree, the rows are INCONCLUSIVE, never PASS.
        LOW BAND (< 300 Hz, "mechanical rumble region"): the same metrics, whole window, rows "(L) ...". They never
          FAIL a capture; a low-band row that does not PASS makes the capture REVIEW (not PASS) unless the excess
          is explained by rumble bursts within the baseline's (all of): low-band bursts (1 s blocks > the median
          block + --lim-quiet-db, +- --lim-quiet-guard-blocks) no more, and no longer in total, than in the
          baseline with the most; not periodic (interval CV, as for clicks); no burst periodic at one DMA block
          (128 ms autocorrelation > --lim-quiet-dma-corr); and the low-band rows recomputed without the burst
          seconds (baselines alike, >= --lim-quiet-min-s left) do not FAIL (INCONCLUSIVE - within baselines that
          disagree, as night low bands do - is accepted). The explanation never touches the high band.
        Verdict: FAIL / INCONCLUSIVE from the high band and continuity first, then REVIEW from the low band, else PASS.
      Continuity (e) is unchanged (whole file). Tone phase steps use the high band for tones >= 300 Hz.

Usage:
  rf_audio_check.py --baseline C0_1.wav --baseline C0_2.wav --capture C3=REC_0012.wav [--capture C1=...]
                    [--bench-log ring.log | --bench-log LABEL=btbench_LABEL.log] [--labels labels.txt]
                    [--no-device-counters LABEL] [--signal auto|silence|tone] [--band-split]
                    [--json out.json] [--md out.md]
  (a capture may name its device file: --capture C1_1=C1_1.wav@REC_0033.wav)
  rf_audio_check.py --self-test
Exit status: 0 all captures pass, 1 a capture fails, is inconclusive or needs review, 2 usage / input error. numpy only (scipy is not needed).

SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import argparse
import contextlib
import io
import json
import math
import re
import sys
import tempfile
import wave
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

FS = 16000
BLOCK = 2048                     # DMA block (128 ms): the recorder's overrun fill and the length tolerance
BAND = (20.0, 7990.0)            # analysis band (Nyquist 8 kHz)
SPUR_WIN_HZ = 2.0
TONE_EXCL_HZ = 6.0               # the Blackman-Harris main lobe (+-4 bins) plus a margin
ENV_FS = 100.0                   # 10 ms envelope
ENV_SEG = 1280                   # 12.8 s envelope segments (0.078 Hz bins; 0.78125 Hz = 1 / 1.28 s is bin 10)
FOLD_PERIOD = 128                # 1.28 s in envelope samples
MOD_TARGETS = (0.78125, 2.34375, 7.5)
ENV_BAND = (0.2, 40.0)           # generic envelope lines: >= 2.5 periods per 12.8 s segment .. below the 45 Hz low-pass


@dataclass
class Limits:
    floor_db: float = 0.5
    spur_db: float = 3.0
    peak_prom_db: float = 6.0
    mod_db: float = 3.0
    mod_prom_db: float = 6.0
    fold_db: float = 3.0
    tone_db: float = 0.5
    phase_rad: float = 0.05
    zero_run: int = BLOCK
    len_tol: int = BLOCK
    drift_smp: int = 8               # block clock: samples outside the C0 captures' drift range
    drift_ppm: float = 300.0         # block clock without C0 counters: gross rate check only
    base_spread_db: float = 0.5      # baseline repeats: floor spread above this -> INCONCLUSIVE
    base_bin_spread_db: float = 1.0  # baseline repeats: median PSD level offset spread above this -> INCONCLUSIVE
    tone_level_db: float = 0.3       # tone level vs the baseline: outside -> INCONCLUSIVE
    noise_db: float = 0.5            # tone captures: residual noise (dBFS) above the baseline
    cont_edge_s: float = 0.5         # continuity checks skip this much at each end (REC switch click)
    split_hz: float = 300.0          # --band-split: the band edge = the FIR's stop edge (nothing below enters the high band)
    split_pass_hz: float = 330.0     # ... the FIR's pass edge (300-330 Hz: the transition)
    split_att_db: float = 100.0      # ... FIR stop-band attenuation (Kaiser window)
    click_margin: int = 2            # --band-split clicks: more than the baselines' most by this -> FAIL (less: INCONCL.)
    second_db: float = 1.0           # --band-split: the loudest 1 s of a band / octave above the baselines' loudest
    click_loud_db: float = 10.0      # --band-split: a click this far above the baselines' loudest -> FAIL
    click_db: float = 20.0           # --band-split click: a 1 ms frame of the high band this far above the median frame
    quiet_db: float = 6.0            # --band-split low-band bursts: 1 s blocks above the median block by this
    quiet_guard_blocks: int = 1      # ... plus this many blocks on each side (decay tails)
    quiet_min_s: float = 30.0        # ... the low-band recheck without the bursts needs this many seconds left
    quiet_dma_corr: float = 0.3      # ... a burst autocorrelated at one DMA block (128 ms) by more is not rumble
    periodic_cv: float = 0.2         # burst / click intervals with a coefficient of variation below this: periodic
    periodic_min_n: int = 4          # ... with at least this many events

# ------------------------------------------------------------------------------------------------------------- io
def load_wav(path: Path) -> tuple[int, np.ndarray]:
    """(rate, samples[n, ch] as float in [-1, 1)). 16-bit PCM only (what the recorder writes)."""
    with wave.open(str(path), "rb") as w:
        if w.getsampwidth() != 2 or w.getcomptype() != "NONE":
            raise ValueError(f"{path}: not 16-bit PCM")
        ch, rate, n = w.getnchannels(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    return rate, x.reshape(-1, ch)


def save_wav(path: Path, x: np.ndarray, rate: int = FS) -> None:
    x = np.atleast_2d(x.T).T if x.ndim == 1 else x
    q = np.clip(np.round(x * 32768.0), -32768, 32767).astype("<i2")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(x.shape[1])
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(q.tobytes())


def parse_bench_log(paths: list[Path]) -> dict[str, dict[str, str]]:
    """Device counters per WAV file name (upper case) from "BENCH rec ...", "BENCH final rec ...", "BENCH meas ..."
    or "+XREC:DBG,REC,i,..." / "+XREC:DBG,LIVE,..." lines. A finished record (rec=0) beats a mid-recording one
    (rec=1: BENCH meas / DBG,LIVE while recording), whatever the order of the files; among equals the last wins."""
    out: dict[str, dict[str, str]] = {}
    prio: dict[str, int] = {}
    for p in paths:
        for line in p.read_text(errors="replace").splitlines():
            if "file=" not in line or ("BENCH" not in line and "+XREC:DBG" not in line):
                continue
            kv = dict(t.split("=", 1) for t in re.split(r"[ ,]+", line.strip()) if "=" in t)
            f = kv.get("file", "-")
            if f in ("", "-"):
                continue
            key, pr = f.upper(), 1 if kv.get("rec") == "0" else 0
            if pr >= prio.get(key, -1):
                out[key], prio[key] = kv, pr
    return out


# ----------------------------------------------------------------------------------------------------- spectra
def bh4(n: int) -> np.ndarray:
    """4-term Blackman-Harris (sidelobes -92 dB): a -20 dBFS tone that is not bin-centred leaks nothing 6 Hz away (Hann
    leaks 10-35 dB above a -70 dBFS floor there, and the leakage moves with a 0.02 Hz tone frequency change)."""
    k = 2 * np.pi * np.arange(n) / (n - 1)
    return 0.35875 - 0.48829 * np.cos(k) + 0.14128 * np.cos(2 * k) - 0.01168 * np.cos(3 * k)


def welch(x: np.ndarray, fs: float, nper: int, window: str = "bh4",
          mask: np.ndarray | None = None) -> tuple[np.ndarray, np.ndarray]:
    """One-sided PSD (density), Blackman-Harris (or Hann), 50 % overlap, mean removed per segment. mask (per sample,
    True = usable): only segments that lie wholly in usable samples are averaged (all of them if none does)."""
    nper = min(nper, len(x))
    hop = max(1, nper // 2)
    w = bh4(nper) if window == "bh4" else np.hanning(nper)
    nseg = 1 + (len(x) - nper) // hop
    idx = np.arange(nper)[None, :] + hop * np.arange(nseg)[:, None]
    if mask is not None:
        bad = np.r_[0, np.cumsum(~np.asarray(mask[:len(x)], dtype=bool))]
        st = hop * np.arange(nseg)
        ok = bad[st + nper] == bad[st]
        if ok.any():
            idx = idx[ok]
    seg = x[idx]
    seg = (seg - seg.mean(axis=1, keepdims=True)) * w
    p = (np.abs(np.fft.rfft(seg, axis=1)) ** 2).mean(axis=0) / (fs * (w ** 2).sum())
    p[1:-1] *= 2.0
    return np.fft.rfftfreq(nper, 1.0 / fs), p


def a_weight(f: np.ndarray) -> np.ndarray:
    """IEC 61672 A-weighting, power gain (normalised to 0 dB at 1 kHz)."""
    f2 = np.maximum(f, 1e-9) ** 2
    ra = (12194.0 ** 2 * f2 ** 2) / ((f2 + 20.6 ** 2) * np.sqrt((f2 + 107.7 ** 2) * (f2 + 737.9 ** 2)) *
                                     (f2 + 12194.0 ** 2))
    return (ra / 0.7943282347242815) ** 2   # 20*log10(RA(1 kHz)) = -2.0 dB


def db(p: float | np.ndarray) -> float | np.ndarray:
    return 10.0 * np.log10(np.maximum(p, 1e-30))


def tone_mask(f: np.ndarray, f0: float | None) -> np.ndarray:
    """True where a bin is NOT near a harmonic of the tone."""
    if not f0:
        return np.ones_like(f, dtype=bool)
    k = np.round(f / f0)
    return ~((k >= 1) & (np.abs(f - k * f0) <= TONE_EXCL_HZ))


SPUR_BASES = ((1600.0, 99), (800.0, 99), (400.0, 99), (800.0 / 3.0, 99), (13000.0 / 60.0, 99), (40.0, 5))
# k*1600 / 800 / 400 Hz (BT slot pairs), k*266.67 Hz (eSCO 3.75 ms), k*216.67 Hz (a 4.615 ms TDMA frame), 40 Hz


def spur_targets(lo: float = BAND[0], hi: float = BAND[1]) -> list[tuple[float, str]]:
    seen: dict[float, str] = {}
    for base, kmax in SPUR_BASES:
        for k in range(1, kmax + 1):
            f = base * k
            if f >= min(hi, BAND[1]):
                break
            if f < lo:
                continue
            key = round(f, 1)
            seen.setdefault(key, f"{k}x{base:.6g}" if k > 1 else f"{base:.6g}")
    return sorted(seen.items())


def local_median(p_db: np.ndarray, f: np.ndarray, half_hz: float = 50.0, excl_hz: float = 3.0) -> np.ndarray:
    """Median of the PSD (dB) within +-half_hz of every bin, excluding +-excl_hz around it."""
    n = len(p_db)
    out = np.empty(n)
    df = float(f[1] - f[0]) if len(f) > 1 else 1.0
    h, e = int(round(half_hz / df)), int(round(excl_hz / df))
    for i in range(n):
        lo, hi = max(0, i - h), min(n, i + h + 1)
        sel = np.r_[p_db[lo:max(lo, i - e)], p_db[min(hi, i + e + 1):hi]]
        out[i] = np.median(sel) if len(sel) else p_db[i]
    return out


# ----------------------------------------------------------------------------------------------------- per channel
def detect_tone(x: np.ndarray) -> float | None:
    """The test tone's frequency, or None (a line 100 Hz - 4 kHz above -50 dBFS, 30 dB above the median bin)."""
    n = min(len(x), 8 * FS)
    w = np.hanning(n)
    spec = np.abs(np.fft.rfft((x[:n] - x[:n].mean()) * w))
    f = np.fft.rfftfreq(n, 1.0 / FS)
    band = (f >= 100) & (f <= 4000)
    i = int(np.argmax(np.where(band, spec, 0.0)))
    level = 20 * np.log10(max(spec[i], 1e-30) * 2.0 / w.sum())
    if level < -50.0 or db(spec[i] ** 2) - db(np.median(spec[band]) ** 2) < 30.0:
        return None
    a, b, c = np.log(spec[i - 1: i + 2] + 1e-30)
    off = 0.5 * (a - c) / (a - 2 * b + c) if (a - 2 * b + c) != 0 else 0.0
    return float((i + off) * FS / n)


def block_fit(x: np.ndarray, f0: float, blk: int) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Least-squares a*cos + b*sin + c per block at f0: (phase, amplitude, residual)."""
    nb = len(x) // blk
    t = np.arange(blk) / FS
    basis = np.stack([np.cos(2 * np.pi * f0 * t), np.sin(2 * np.pi * f0 * t), np.ones(blk)], axis=1)
    pinv = np.linalg.pinv(basis)
    ph, amp, res = np.empty(nb), np.empty(nb), np.empty(nb * blk)
    for k in range(nb):
        seg = x[k * blk:(k + 1) * blk]
        t0 = k * blk / FS
        a, b, c = pinv @ seg
        res[k * blk:(k + 1) * blk] = seg - basis @ np.array([a, b, c])
        ph[k] = math.atan2(-b, a) - 2 * np.pi * f0 * t0     # phase constant of cos(2 pi f0 T + ph), T = file time
        amp[k] = math.hypot(a, b)
    return ph, amp, res


def tone_fit(x: np.ndarray, f0: float, blk: int, iters: int = 4) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    """Per block: a 4-parameter sine fit (IEEE 1057: amplitude, phase, offset and the block's own frequency, from f0
    by Gauss-Newton), so a tone that is off or wanders by a few 10 ppm leaves no residual: (freq, amplitude,
    residual). A fixed-frequency fit reads a 0.01 Hz error over 1 s as about 5 dB of extra noise."""
    nb = len(x) // blk
    t = (np.arange(blk) - (blk - 1) / 2.0) / FS        # centred: the frequency term is orthogonal to the phase
    ones = np.ones(blk)
    fr, amp, res = np.empty(nb), np.empty(nb), np.empty(nb * blk)
    for k in range(nb):
        seg = x[k * blk:(k + 1) * blk]
        w = 2 * np.pi * f0
        for _ in range(iters):
            c, s_ = np.cos(w * t), np.sin(w * t)
            a, b, _dc = np.linalg.lstsq(np.stack([c, s_, ones], axis=1), seg, rcond=None)[0]
            jac = t * (-a * s_ + b * c)
            dw = np.linalg.lstsq(np.stack([c, s_, ones, jac], axis=1), seg, rcond=None)[0][3]
            w += float(np.clip(dw, -2 * np.pi * 2.0, 2 * np.pi * 2.0))   # at most 2 Hz per step
        c, s_ = np.cos(w * t), np.sin(w * t)
        basis = np.stack([c, s_, ones], axis=1)
        coef = np.linalg.lstsq(basis, seg, rcond=None)[0]
        res[k * blk:(k + 1) * blk] = seg - basis @ coef
        fr[k] = w / (2 * np.pi)
        amp[k] = math.hypot(coef[0], coef[1])
    return fr, amp, res


def phase_steps(x: np.ndarray, f0: float, limit: float, t_off: float = 0.0) -> list[dict]:
    """Phase steps of the tone between 16 ms blocks: a single-block step, or one split across two blocks (the loss
    fell inside a block: each diff carries part of it). Adjacent hits are one event."""
    blk = 256
    ph, amp, _ = block_fit(x, f0, blk)
    if len(ph) < 8:
        return []
    d = np.angle(np.exp(1j * np.diff(ph)))      # constant for a continuous tone (f0 error: a slow, smooth drift)
    k = 31
    pad = np.pad(d, k, mode="edge")
    med = np.array([np.median(pad[i:i + 2 * k + 1]) for i in range(len(d))])
    step = d - med
    ok = (amp[1:] > 0.3 * np.median(amp)) & (amp[:-1] > 0.3 * np.median(amp))
    pair = np.zeros_like(step)
    pair[:-1] = step[:-1] + step[1:]
    okp = np.zeros_like(ok)
    okp[:-1] = ok[:-1] & ok[1:]
    hit = ((np.abs(step) > limit) & ok) | ((np.abs(pair) > limit) & okp)
    out: list[dict] = []
    last = -10
    for i in np.nonzero(hit)[0]:
        v = float(step[i]) if abs(step[i]) >= abs(pair[i]) else float(pair[i])
        if i - last <= 2 and out:
            if abs(v) > abs(out[-1]["rad"]):
                out[-1]["rad"] = round(v, 3)
        else:
            out.append({"t_s": round(t_off + (i + 1) * blk / FS, 3), "rad": round(v, 3)})
        last = i
        if len(out) >= 50:
            break
    return out


def zero_runs(x2d: np.ndarray, min_len: int) -> list[dict]:
    z = np.all(x2d == 0.0, axis=1).astype(np.int8)
    edges = np.diff(np.r_[0, z, 0])
    starts, ends = np.nonzero(edges == 1)[0], np.nonzero(edges == -1)[0]
    return [{"t_s": round(s / FS, 3), "samples": int(e - s)} for s, e in zip(starts, ends) if e - s >= min_len]


def envelope(x: np.ndarray, mask: np.ndarray | None = None) -> np.ndarray:
    """RMS envelope at ENV_FS: x^2 low-passed below 45 Hz (brick wall), then decimated. A plain 10 ms mean would alias
    a tone's 2 f0 power ripple into the envelope band (997 Hz -> a 6 Hz line that moves with the tone frequency).
    mask (True = usable): x^2 is held at the usable samples' mean elsewhere (an excluded burst would otherwise ring
    through the brick-wall filter for seconds; holding keeps the time base for the 1.28 s fold)."""
    m = int(FS / ENV_FS)
    n = len(x) // m
    p = x[:n * m] ** 2
    if mask is not None and not mask[:n * m].all() and mask[:n * m].any():
        p = p.copy()
        mk = mask[:n * m]
        p[~mk] = p[mk].mean()
    spec = np.fft.rfft(p)
    spec[np.fft.rfftfreq(len(p), 1.0 / FS) > 0.45 * ENV_FS] = 0.0
    lp = np.fft.irfft(spec, len(p))
    return np.sqrt(np.maximum(lp[m // 2::m][:n], 0.0))


def env_metrics(x: np.ndarray, mask: np.ndarray | None = None) -> dict:
    e = envelope(x, mask)
    mean = float(e.mean())
    if len(e) < 2 * FOLD_PERIOD or mean <= 0:
        return {}
    ed = e - np.polyval(np.polyfit(np.arange(len(e)), e, 1), np.arange(len(e)))
    f, p = welch(ed, ENV_FS, min(ENV_SEG, len(ed)), window="hann")
    df = f[1] - f[0]
    pdb = db(p * df / mean ** 2)                                 # modulation power re the mean envelope
    excl = np.zeros_like(f, dtype=bool)
    for ft in MOD_TARGETS:
        excl |= np.abs(f - ft) <= 0.2
    ref = (f >= 0.3) & (f <= 12.0) & ~excl
    med = float(np.median(pdb[ref])) if ref.any() else float("nan")
    mod = {}
    for ft in MOD_TARGETS:
        sel = np.abs(f - ft) <= 0.1
        lvl = float(pdb[sel].max())
        mod[f"{ft:g}"] = {"db": round(lvl, 2), "prom_db": round(lvl - med, 2)}

    def fold_depth(period: int) -> float:
        nper = len(ed) // period
        prof = ed[:nper * period].reshape(nper, period).mean(axis=0)
        return 20 * math.log10(max(float(prof.std()), 1e-30) / mean)

    fold = fold_depth(FOLD_PERIOD)
    # what folding this same envelope at periods that are not 1.28 s gives: the noise and any unrelated rhythm
    noise = float(np.median([fold_depth(p) for p in (119, 123, 125, 131, 133, 137)]))
    return {"mod": mod, "fold_db": round(fold, 2), "fold_noise_db": round(noise, 2),
            "_f": f, "_pdb": pdb, "_med": local_median(pdb, f, half_hz=2.0, excl_hz=0.3)}


_AUTO = object()


def band_octaves(band: tuple[float, float]) -> list[tuple[float, float]]:
    """Octaves from the band's lower edge (the last one ends at the band's upper edge)."""
    out, lo = [], band[0]
    while lo * 2 < band[1]:
        out.append((lo, lo * 2))
        lo *= 2
    out.append((lo, band[1]))
    return out


def loudest_second(x: np.ndarray, band: tuple[float, float], mask: np.ndarray | None,
                   f0: float | None) -> dict[str, float]:
    """The loudest 1 s (Hann, 0.5 s hop) of the band and of each of its octaves, dBFS: a short event that the
    whole-window floor dilutes (1 s at the floor's own level in one octave: about +2 dB here). mask: only windows
    wholly in usable samples. f0: the tone's harmonics are left out."""
    n = FS
    st = np.arange(0, len(x) - n + 1, n // 2)
    if mask is not None:
        bad = np.r_[0, np.cumsum(~np.asarray(mask[:len(x)], dtype=bool))]
        ok = bad[st + n] == bad[st]
        st = st[ok] if ok.any() else st
    w = np.hanning(n)
    f = np.fft.rfftfreq(n, 1.0 / FS)
    keep = tone_mask(f, f0)
    bands = [(band[0], band[1])] + band_octaves(band)
    sel = [keep & (f >= lo) & (f < hi) for lo, hi in bands]
    best = np.full(len(bands), -np.inf)
    norm = 2.0 / (FS * (w ** 2).sum())
    for i in range(0, len(st), 64):
        seg = x[st[i:i + 64, None] + np.arange(n)[None, :]]
        sp = np.abs(np.fft.rfft((seg - seg.mean(axis=1, keepdims=True)) * w, axis=1)) ** 2 * norm
        for j, sl in enumerate(sel):
            best[j] = max(best[j], float(db(sp[:, sl].sum(axis=1)).max()))
    return {f"{lo:g}-{hi:g}": round(float(v), 2) for (lo, hi), v in zip(bands, best)}


def analyse_channel(x: np.ndarray, signal: str, tone_hz: float | None, lim: Limits,
                    mask: np.ndarray | None = None, band: tuple[float, float] = BAND, f0=_AUTO,
                    harm_f0: float | None = None) -> dict:
    """One channel (or one band of it: x already filtered, band its edges). f0: the tone in this band (None: a
    silence band), default detected; harm_f0: a tone outside the band whose harmonics are kept out of the band's
    floor and lines. mask (per sample, True = usable): Welch / envelope / tone blocks only from usable samples."""
    out: dict = {}
    if f0 is _AUTO:
        f0 = None
        if signal in ("auto", "tone"):
            f0 = tone_hz or detect_tone(x)
            if signal == "tone" and f0 is None:
                raise ValueError("--signal tone, but no tone found (give --tone-hz)")
    out["signal"] = "tone" if f0 else "silence"
    out["tone_hz"] = round(f0, 3) if f0 else None
    out["band_hz"] = [band[0], band[1]]
    hf0 = f0 or harm_f0
    f, p = welch(x, FS, FS, mask=mask)
    inb = (f >= band[0]) & (f <= band[1])
    keep = inb & tone_mask(f, hf0)
    if not f0:
        fl = inb & tone_mask(f, harm_f0)
        out["floor_db"] = round(float(db(p[fl].sum())), 3)
        out["floor_a_db"] = round(float(db((p * a_weight(f))[fl].sum())), 3)
        if band != BAND:
            out["floor_oct_db"] = {f"{lo:g}-{hi:g}": round(float(db(p[fl & (f >= lo) & (f < hi)].sum())), 3)
                                   for lo, hi in band_octaves(band)}
            out["second_db"] = loudest_second(x, band, mask, harm_f0)
    else:
        fr, amp, res = tone_fit(x, f0, FS)
        if mask is not None:
            kb = np.array([mask[k * FS:(k + 1) * FS].all() for k in range(len(amp))], dtype=bool)
            if kb.any():
                fr, amp = fr[kb], amp[kb]
        fr_, pr = welch(res, FS, FS, mask=None if mask is None else mask[:len(res)])
        bandr = (fr_ >= band[0]) & (fr_ <= band[1])
        p_fund = float(np.mean(amp ** 2) / 2.0)
        p_thdn = float(pr[bandr].sum())
        p_noise = float(pr[bandr & tone_mask(fr_, f0)].sum())
        out["tone_dbfs"] = round(float(db(p_fund)), 3)
        out["noise_dbfs"] = round(float(db(p_noise)), 3)
        out["snr_db"] = round(float(db(p_fund) - db(p_noise)), 3)
        out["thdn_db"] = round(float(db(p_thdn) - db(p_fund)), 3)
        out["tone_wander_hz"] = round(float(fr.max() - fr.min()), 4) if len(fr) else 0.0
    out["_psd_db"] = db(p)
    out["_p"] = p
    out["_keep"] = keep
    out["_f"] = f
    out["env"] = env_metrics(x, mask)
    return out


# ----------------------------------------------------------------------------------------------------- compare
@dataclass
class Capture:
    label: str
    path: Path
    rate: int = 0
    n: int = 0
    ch: list[dict] = field(default_factory=list)
    raw: np.ndarray | None = None
    split: dict | None = None        # --band-split: the low-band bursts (all channels) and their mask


def notch_tone(x: np.ndarray, f0: float, half_hz: float = 20.0) -> np.ndarray:
    """x without the tone and its harmonics (+-half_hz, FFT over the whole signal)."""
    spec = np.fft.rfft(x)
    f = np.fft.rfftfreq(len(x), 1.0 / FS)
    k = np.round(f / f0)
    spec[(k >= 1) & (np.abs(f - k * f0) <= half_hz)] = 0.0
    return np.fft.irfft(spec, len(x))


_FIR_CACHE: dict[tuple[float, float, float], np.ndarray] = {}


def split_fir(lim: Limits) -> np.ndarray:
    """Linear-phase FIR high-pass (Kaiser-windowed sinc, odd length): stop below lim.split_hz with lim.split_att_db
    attenuation, pass from lim.split_pass_hz (300 / 330 Hz / 100 dB: 3419 taps, 107 ms each side). Rumble below
    300 Hz thus never reaches the high band (near full-scale rumble ends up ~20 dB under the quiet floor)."""
    key = (lim.split_hz, lim.split_pass_hz, lim.split_att_db)
    if key not in _FIR_CACHE:
        if not 0 < lim.split_hz < lim.split_pass_hz < BAND[1]:
            raise ValueError("--lim-split-pass-hz must lie above --lim-split-hz")
        att = lim.split_att_db
        dw = 2 * np.pi * (lim.split_pass_hz - lim.split_hz) / FS
        n = int(math.ceil((att - 8.0) / (2.285 * dw))) | 1
        beta = 0.1102 * (att - 8.7) if att > 50 else 0.5842 * (att - 21) ** 0.4 + 0.07886 * (att - 21)
        m = np.arange(n) - (n - 1) / 2.0
        fc = (lim.split_hz + lim.split_pass_hz) / 2.0 / FS
        h = -2 * fc * np.sinc(2 * fc * m) * np.kaiser(n, beta)
        h[(n - 1) // 2] += 1.0
        _FIR_CACHE[key] = h
    return _FIR_CACHE[key]


def fir_highpass(x: np.ndarray, lim: Limits) -> np.ndarray:
    """x (1-D, the whole file) through split_fir, delay-compensated (zero phase, same length; mean removed first so
    the file edges do not step)."""
    h = split_fir(lim)
    x = x - x.mean()
    n = len(x) + len(h) - 1
    nfft = 1 << (n - 1).bit_length()
    y = np.fft.irfft(np.fft.rfft(x, nfft) * np.fft.rfft(h, nfft), nfft)[:n]
    d = (len(h) - 1) // 2
    return y[d:d + len(x)]


def quiet_blocks(xw: np.ndarray, lim: Limits, f0s: list[float | None] | None = None) -> dict:
    """--band-split low-band bursts: 1 s blocks of the analysis window (all channels) whose RMS exceeds the channel's
    median block by lim.quiet_db, widened by lim.quiet_guard_blocks on each side. f0s: a channel's test tone, notched
    out first. Returns the per-sample mask (True = not in a burst) and the bursts (runs of blocks) as sample ranges of
    the window, with the time of each burst's loudest 10 ms."""
    n, nch = xw.shape
    xd = xw.copy()
    for k, f0 in enumerate(f0s or []):
        if f0:
            xd[:, k] = notch_tone(xw[:, k], f0)
    nb = -(-n // FS)
    lv = np.empty((nb, nch))
    for i in range(nb):
        seg = xd[i * FS:(i + 1) * FS]
        lv[i] = db(((seg - seg.mean(axis=0)) ** 2).mean(axis=0))
    med = np.median(lv, axis=0)
    over = lv - med
    core = (over > lim.quiet_db).any(axis=1)
    excl = core.copy()
    g = max(0, int(lim.quiet_guard_blocks))
    for i in np.nonzero(core)[0]:
        excl[max(0, i - g):i + g + 1] = True
    mask = np.repeat(~excl, FS)[:n]
    edges = np.diff(np.r_[0, excl.astype(np.int8), 0])
    fr = FS // 100
    nfr = n // fr
    p10 = ((xd[:nfr * fr] - xd.mean(axis=0)) ** 2).reshape(nfr, fr, nch).mean(axis=1)
    p10 = (p10 / np.maximum(10 ** (med / 10), 1e-30)).max(axis=1)
    bursts = []
    for s0, e0 in zip(np.nonzero(edges == 1)[0], np.nonzero(edges == -1)[0]):
        s, e = int(s0) * FS, min(int(e0) * FS, n)
        a0, a1 = s // fr, max(s // fr + 1, e // fr)
        pk = (a0 + int(np.argmax(p10[a0:min(a1, nfr)]))) * fr if a0 < nfr else s
        bursts.append({"s": s, "e": e, "core_s": int(core[s0:e0].sum()),
                       "peak_db_re_quiet": round(float(over[s0:e0].max()), 1), "pk": pk,
                       "dma_corr": round(max(dma_corr(xd[s:e, k]) for k in range(nch)), 3)})
    return {"mask": mask, "quiet_db": [round(float(v), 2) for v in med], "kept_s": round(float(mask.sum()) / FS, 2),
            "excl_s": round(float((~mask).sum()) / FS, 2), "bursts": bursts}


def dma_corr(seg: np.ndarray) -> float:
    """Periodicity at one DMA block (128 ms, k*7.8125 Hz) rather than mere slowness: the autocorrelation at 2048 +-1
    samples minus the larger of those at 1.5 and 2.5 blocks (a plateau or a slow swell correlates at every lag)."""
    s0 = seg - seg.mean()

    def acorr(lag: int) -> float:
        if len(s0) <= lag + FS // 4:
            return 0.0
        a_, b_ = s0[:-lag], s0[lag:]
        return float(np.dot(a_, b_) / max(math.sqrt(float(np.dot(a_, a_)) * float(np.dot(b_, b_))), 1e-30))

    ac = max(acorr(lag) for lag in (BLOCK - 1, BLOCK, BLOCK + 1))
    return ac - max(0.0, acorr(3 * BLOCK // 2), acorr(5 * BLOCK // 2))


def click_events(x: np.ndarray, lim: Limits) -> list[dict]:
    """Clicks and steps in one high-band channel (x already high-passed, tone notched), over the whole window, no
    exclusion (rumble knocks count here as in the baselines):
      click - a 1 ms frame lim.click_db above the window's median frame;
      step  - the output of a filter matched to the split FIR's step response (a DC step, e.g. a supply droop, puts
              little energy in any 1 ms frame of the high band but all of it on this one shape) lim.click_db above
              its robust RMS (1.4826 * MAD): a 0.002 FS step on a -87 dBFS floor stands about 35 dB above it.
    Events within 50 ms are one. Returns [{"i": first sample, "ex_db"}]."""
    fr = FS // 1000
    nf = len(x) // fr
    pw = (x[:nf * fr].reshape(nf, fr) ** 2).mean(axis=1)
    ex = db(pw) - db(float(np.median(pw)))
    g = np.cumsum(split_fir(lim))
    g = g / max(float(np.sqrt(np.dot(g, g))), 1e-30)
    n = len(x) + len(g) - 1
    nfft = 1 << (n - 1).bit_length()
    y = np.fft.irfft(np.fft.rfft(x, nfft) * np.fft.rfft(g[::-1], nfft), nfft)
    c0 = len(g) - 1 - (len(g) - 1) // 2
    z = y[c0:c0 + len(x)]                     # z[i]: x correlated with g centred on sample i (the step's time)
    sig = 1.4826 * float(np.median(np.abs(z - np.median(z))))
    zdb = 20 * np.log10(np.maximum(np.abs(z), 1e-30) / max(sig, 1e-30))
    zf = zdb[:nf * fr].reshape(nf, fr).max(axis=1)
    ex = np.maximum(ex, np.where(zf >= lim.click_db, zf, -np.inf))
    out: list[dict] = []
    last = -10 ** 9
    for i in np.nonzero(ex >= lim.click_db)[0]:
        if out and i * fr - last <= 50 * fr:
            out[-1]["ex_db"] = max(out[-1]["ex_db"], round(float(ex[i]), 1))
        else:
            out.append({"i": int(i * fr), "ex_db": round(float(ex[i]), 1)})
        last = int(i * fr)
    return out


def load_capture(label: str, path: Path, signal: str, tone_hz: float | None, trim: tuple[float, float],
                 lim: Limits, split: bool = False) -> Capture:
    rate, x = load_wav(path)
    if rate != FS:
        raise ValueError(f"{path}: {rate} Hz, expected {FS}")
    c = Capture(label, path, rate, len(x))
    a, b = int(trim[0] * FS), len(x) - int(trim[1] * FS)
    if b - a < 4 * FS:
        raise ValueError(f"{path}: too short after trimming ({(b - a) / FS:.1f} s, need >= 4 s)")
    c.raw = x
    e = min(int(lim.cont_edge_s * FS), len(x) // 4)
    if not split:
        for k in range(x.shape[1]):
            ch = analyse_channel(x[a:b, k], signal, tone_hz, lim)
            if ch["tone_hz"]:   # continuity: the whole file but the edges (the spectral metrics keep the middle window)
                ch["phase_steps"] = phase_steps(x[e:len(x) - e, k], ch["tone_hz"], lim.phase_rad, e / FS)
            c.ch.append(ch)
        return c
    # --band-split: the same FIR on every file, the whole file filtered, then the analysis window taken
    hi_band, lo_band = (lim.split_hz, BAND[1]), (BAND[0], lim.split_hz)
    xh = np.stack([fir_highpass(x[:, k], lim) for k in range(x.shape[1])], axis=1)
    xl = x - x.mean(axis=0) - xh
    f0s: list[float | None] = []
    for k in range(x.shape[1]):
        f0 = None
        if signal in ("auto", "tone"):
            f0 = tone_hz or detect_tone(x[a:b, k])
            if signal == "tone" and f0 is None:
                raise ValueError("--signal tone, but no tone found (give --tone-hz)")
        f0s.append(f0)
    lo_f0s = [f0 if f0 and f0 < lim.split_hz else None for f0 in f0s]
    c.split = quiet_blocks(xl[a:b], lim, lo_f0s)
    mask = c.split["mask"]
    for k, f0 in enumerate(f0s):
        hi_f0 = f0 if f0 and f0 >= lim.split_hz else None
        ch: dict = {"signal": "tone" if f0 else "silence", "tone_hz": round(f0, 3) if f0 else None}
        ch["hi"] = analyse_channel(xh[a:b, k], signal, tone_hz, lim, band=hi_band, f0=hi_f0, harm_f0=f0)
        ch["lo"] = analyse_channel(xl[a:b, k], signal, tone_hz, lim, band=lo_band, f0=lo_f0s[k], harm_f0=f0)
        ch["lo_q"] = analyse_channel(xl[a:b, k], signal, tone_hz, lim, mask=mask, band=lo_band, f0=lo_f0s[k],
                                     harm_f0=f0)
        xc = notch_tone(xh[a:b, k], f0) if f0 else xh[a:b, k]
        evs = click_events(xc, lim)
        ch["clicks"] = [round((a + ev["i"]) / FS, 3) for ev in evs]
        ch["click_max_db"] = max([ev["ex_db"] for ev in evs], default=None)
        if f0:   # continuity on the whole file but the edges; a tone >= 300 Hz in the high band (rumble removed)
            xs = xh[:, k] if hi_f0 else x[:, k]
            ch["phase_steps"] = phase_steps(xs[e:len(x) - e], f0, lim.phase_rad, e / FS)
        c.ch.append(ch)
    for bu in c.split["bursts"]:
        bu.update({"t_s": round((a + bu["s"]) / FS, 2), "dur_s": round((bu["e"] - bu["s"]) / FS, 2),
                   "t_pk_s": round((a + bu["pk"]) / FS, 2)})
    # information only (no verdict uses it): how much of the high band's energy above its median second lies in the
    # low-band burst seconds - near 1 when the high-band excess is the rumble events' own knocks and rattles
    xn = np.stack([notch_tone(xh[a:b, k], f0) if f0 else xh[a:b, k] for k, f0 in enumerate(f0s)], axis=1)
    nb = len(xn) // FS
    ps = (xn[:nb * FS] ** 2).reshape(nb, FS, -1).mean(axis=(1, 2))
    exc = np.maximum(ps - np.median(ps), 0.0)
    inb = ~mask[:nb * FS].reshape(nb, FS).all(axis=1)
    c.split["hb_excess_in_bursts"] = round(float(exc[inb].sum() / exc.sum()), 3) if exc.sum() > 0 else None
    c.split["hb_loud_s"] = [round((a + i * FS) / FS, 1) for i in np.nonzero(db(ps) - db(np.median(ps)) > 6.0)[0]][:40]
    return c


def row(rows: list, metric: str, value, base, delta, limit: str, ok: bool, ch: int | None = None,
        verdict: str | None = None, band: str | None = None) -> None:
    """band (--band-split): "high", "low" (rumble region), "lowx" (the low band's explanation), "lowv" (its
    verdict) or None (continuity, counters)."""
    rows.append({"metric": metric, "ch": ch, "band": band, "value": value, "baseline": base, "delta": delta,
                 "limit": limit, "verdict": verdict or ("PASS" if ok else "FAIL")})


def rnd(v, n=2):
    return None if v is None else round(float(v), n)


def ref_level(vals: list) -> np.ndarray:
    """Baseline reference: mean + 1 standard deviation of the repeats (does not grow with their number)."""
    a = np.asarray(vals, dtype=float)
    if len(a) < 2:
        return a[0]
    return a.mean(axis=0) + a.std(axis=0, ddof=1)


SETUP_KEYS = ("gain", "dsp", "vor", "ch")


def _bdrift(d: dict | None) -> tuple[int, int] | None:
    try:
        bd, bc = int(d["bdrift"]), int(d["bclk_smp"])   # type: ignore[index]
    except (TypeError, KeyError, ValueError):
        return None
    return (bd, bc) if bc > 0 else None


def _cv(times: list[float], lim: Limits) -> float | None:
    """The lowest coefficient of variation of the intervals of any run of >= lim.periodic_min_n consecutive events
    (a rhythm interleaved with a few unrelated bursts still shows), or None with fewer events."""
    t = np.sort(np.asarray(times, dtype=float))
    n, m = len(t), max(2, lim.periodic_min_n)
    if n < m:
        return None
    d = np.diff(t)
    best = math.inf
    for i in range(len(d) - m + 2):
        c1 = np.cumsum(d[i:])
        c2 = np.cumsum(d[i:] ** 2)
        k = np.arange(1, len(c1) + 1)
        sel = k >= m - 1
        mean = c1[sel] / k[sel]
        var = np.maximum(c2[sel] / k[sel] - mean ** 2, 0.0)
        cv = np.where(mean > 0, np.sqrt(var) / np.maximum(mean, 1e-12), 0.0)
        if len(cv):
            best = min(best, float(cv.min()))
    return best


def band_rows(rows: list, c: dict, bk: list[dict], lim: Limits, k: int, tag: str = "",
              band: str | None = None) -> None:
    """(b) baseline repeatability, (a) floor, (b) spurs / peaks, (c) envelope and (d) tone rows for one channel, or
    one band of it (c: the capture's analyse_channel result, bk: the baselines'). tag prefixes the metric names.
    A baseline that fails its own repeatability is "noisy": then an excess over its mean + sd that stays within its
    loudest repeat (+ the limit) is INCONCLUSIVE, not FAIL - only a capture outside every repeat FAILS."""
    def add(metric: str, *a, **kw) -> None:
        row(rows, tag + metric, *a, ch=k, band=band, **kw)

    def judge(bad: bool, bad_vs_max: bool) -> str | None:
        return "INCONCLUSIVE" if bad and noisy and not bad_vs_max else None

    if not bk:
        return
    noisy = False
    if len(bk) < 2:
        add("(b) baseline repeatability", len(bk), None, None, "needs >= 2 repeats", False, verdict="INCONCLUSIVE")
    else:   # the bars below are only as good as the baseline's own repeatability
        keep = bk[0]["_keep"]
        psd = np.array([b["_psd_db"][keep] for b in bk])
        off = np.median(psd - psd.mean(axis=0), axis=1)
        sp = float(off.max() - off.min())
        noisy = sp > lim.base_bin_spread_db
        add("(b) baseline repeatability", rnd(sp), None, None,
            f"median level offset spread <= {lim.base_bin_spread_db} dB", True,
            verdict=None if sp <= lim.base_bin_spread_db else "INCONCLUSIVE")
    lo_hz, hi_hz = c.get("band_hz", BAND)
    bname = "" if (lo_hz, hi_hz) == BAND else f" {lo_hz:g}-{hi_hz:g} Hz"
    # (a) noise floor (and its octaves in a split band)
    if c["signal"] == "silence":
        keys = [("floor_db", "(a) noise floor" + bname, None), ("floor_a_db", "(a) noise floor A-wtd" + bname, None)]
        keys += [("floor_oct_db", f"(a) floor octave {o} Hz", o) for o in c.get("floor_oct_db", {})]
        for o in c.get("second_db", {}):     # the loudest second: above every repeat FAILS
            vals = [b["second_db"][o] for b in bk]
            cv_ = c["second_db"][o]
            d = cv_ - max(vals)
            add(f"(a) loudest second {o} Hz", rnd(cv_), rnd(max(vals)), rnd(d),
                f"<= the baselines' loudest + {lim.second_db:g} dB", d <= lim.second_db)
        for key, name, sub in keys:
            vals = [b[key] if sub is None else b[key][sub] for b in bk]
            cv_ = c[key] if sub is None else c[key][sub]
            m = float(np.mean(vals))
            d = cv_ - m
            sp = max(vals) - min(vals)
            v = None
            if sub is not None:                  # octaves: an added artefact raises one; one-sided
                if d > lim.floor_db + sp:
                    v = "FAIL"
                elif d > lim.floor_db:
                    v = "INCONCLUSIVE"           # above the bar, within the repeats' own spread
                add(name, rnd(cv_), rnd(m), rnd(d), f"<= +{lim.floor_db} dB (spread {sp:.2f}: rise within it "
                    "INCONCLUSIVE)", d <= lim.floor_db, verdict=v)
                continue
            if abs(d) > lim.floor_db + sp:
                v = "FAIL"                       # outside even the repeats' own spread
            elif sp > lim.base_spread_db:
                v = "INCONCLUSIVE"               # the baseline cannot support a +-floor_db bar
            add(name, rnd(cv_), rnd(m), rnd(d), f"+-{lim.floor_db} dB (spread {sp:.2f})", abs(d) <= lim.floor_db,
                verdict=v)
    # (b) spurs and new peaks
    f = c["_f"]
    env = ref_level([b["_psd_db"] for b in bk])
    pmax = np.max([b["_psd_db"] for b in bk], axis=0)
    keep = c["_keep"]
    worst = None
    fails = []
    over_max = False
    for ft, name in spur_targets(lo_hz, hi_hz):
        sel = (np.abs(f - ft) <= SPUR_WIN_HZ) & keep
        if not sel.any():
            continue
        d = float(c["_psd_db"][sel].max() - env[sel].max())
        if worst is None or d > worst[1]:
            worst = (name, d)
        if d > lim.spur_db:
            fails.append(f"{name} Hz +{d:.1f} dB")
            over_max |= float(c["_psd_db"][sel].max() - pmax[sel].max()) > lim.spur_db
    if worst is not None:
        add("(b) spur targets", f"worst {worst[0]} Hz", "C0 mean+sd", rnd(worst[1]), f"<= +{lim.spur_db} dB",
            not fails, verdict=judge(bool(fails), over_max))
        if fails:
            rows[-1]["detail"] = fails[:20]
    pdb = c["_psd_db"]
    med = local_median(pdb, f)
    hit = keep & (pdb - env > lim.spur_db) & (pdb - med >= lim.peak_prom_db)
    peaks = [{"hz": float(f[i]), "excess_db": rnd(pdb[i] - env[i]), "prom_db": rnd(pdb[i] - med[i])}
             for i in np.nonzero(hit)[0][:20]]
    add("(b) new narrow peaks", len(peaks), 0, None,
        f"none > +{lim.spur_db} dB with >= {lim.peak_prom_db} dB prominence", not peaks,
        verdict=judge(bool(peaks), bool((hit & (pdb - pmax > lim.spur_db)).any())))
    if peaks:
        rows[-1]["detail"] = peaks
    # (c) envelope modulation
    if c["env"] and all(b["env"] for b in bk):
        for ft, m in c["env"]["mod"].items():
            bref = float(ref_level([b["env"]["mod"][ft]["db"] for b in bk]))
            d = m["db"] - bref
            ok = not (d > lim.mod_db and m["prom_db"] >= lim.mod_prom_db)
            dmax = m["db"] - max(b["env"]["mod"][ft]["db"] for b in bk)
            add(f"(c) envelope {ft} Hz", rnd(m["db"]), rnd(bref), rnd(d),
                f"line: <= +{lim.mod_db} dB or prom < {lim.mod_prom_db} dB (prom {m['prom_db']:.1f})", ok,
                verdict=judge(not ok, dmax > lim.mod_db))
        ef, ep, emed = c["env"]["_f"], c["env"]["_pdb"], c["env"]["_med"]
        if all(len(b["env"]["_pdb"]) == len(ep) for b in bk):
            eref = ref_level([b["env"]["_pdb"] for b in bk])
            sel = (ef >= ENV_BAND[0]) & (ef <= ENV_BAND[1])
            hit = sel & (ep - eref > lim.mod_db) & (ep - emed >= lim.mod_prom_db)
            lines = [{"hz": round(float(ef[i]), 3), "excess_db": rnd(ep[i] - eref[i]),
                      "prom_db": rnd(ep[i] - emed[i])} for i in np.nonzero(hit)[0][:20]]
            emax = np.max([b["env"]["_pdb"] for b in bk], axis=0)
            add("(c) envelope lines", len(lines), 0, None,
                f"none {ENV_BAND[0]:g}-{ENV_BAND[1]:g} Hz > +{lim.mod_db} dB with >= {lim.mod_prom_db} dB prom",
                not lines, verdict=judge(bool(lines), bool((hit & (ep - emax > lim.mod_db)).any())))
            if lines:
                rows[-1]["detail"] = lines
        bf = max(float(ref_level([b["env"]["fold_db"] for b in bk])), c["env"]["fold_noise_db"])
        d = c["env"]["fold_db"] - bf
        dmax = c["env"]["fold_db"] - max(max(b["env"]["fold_db"] for b in bk), c["env"]["fold_noise_db"])
        add("(c) 1.28 s fold depth", rnd(c["env"]["fold_db"]), rnd(bf), rnd(d), f"<= +{lim.fold_db} dB",
            d <= lim.fold_db, verdict=judge(d > lim.fold_db, dmax > lim.fold_db))
    # (d) tone
    if c["signal"] == "tone":
        bl = float(np.mean([b["tone_dbfs"] for b in bk]))
        dl = c["tone_dbfs"] - bl
        add("(d) tone level", rnd(c["tone_dbfs"]), rnd(bl), rnd(dl),
            f"+-{lim.tone_level_db} dB (else SNR moves with level)", True,
            verdict=None if abs(dl) <= lim.tone_level_db else "INCONCLUSIVE")
        bs = float(np.mean([b["snr_db"] for b in bk]))
        bt = float(np.mean([b["thdn_db"] for b in bk]))
        bn = float(np.mean([b["noise_dbfs"] for b in bk]))
        add("(d) SNR", rnd(c["snr_db"]), rnd(bs), rnd(c["snr_db"] - bs), f">= -{lim.tone_db} dB",
            c["snr_db"] - bs >= -lim.tone_db)
        add("(d) THD+N", rnd(c["thdn_db"]), rnd(bt), rnd(c["thdn_db"] - bt), f"<= +{lim.tone_db} dB",
            c["thdn_db"] - bt <= lim.tone_db)
        add("(d) residual noise dBFS", rnd(c["noise_dbfs"]), rnd(bn), rnd(c["noise_dbfs"] - bn),
            f"<= +{lim.noise_db} dB", c["noise_dbfs"] - bn <= lim.noise_db)


LOW_BANDS = ("low", "lowx", "lowv")   # --band-split rows that never FAIL a capture (REVIEW at most)


def worst_verdict(vs) -> str:
    vs = set(vs)
    for v in ("FAIL", "INCONCLUSIVE", "REVIEW"):
        if v in vs:
            return v
    return "PASS"


def split_rows(rows: list, cap: Capture, base: list[Capture], lim: Limits, is_base: bool) -> None:
    """--band-split: (c) clicks in the high band (whole window) and the low band's verdict (REVIEW unless the
    excess is explained by rumble bursts within the baseline's)."""
    for k, c in enumerate(cap.ch):
        n = len(c["clicks"])
        cv = _cv(c["clicks"], lim)
        if is_base:
            row(rows, "(c) clicks (high band)", n, None, None, "baseline: counted for the captures' bar", True, k,
                band="high")
        elif base:   # counts scaled to this capture's length
            bn = [len(b.ch[k]["clicks"]) * cap.n / b.n for b in base]
            hi_, lo_ = math.ceil(max(bn) - 1e-9), math.floor(min(bn) + 1e-9)
            if n > hi_ + lim.click_margin:
                v = "FAIL"                     # beyond every repeat by more than the margin
            elif n > hi_ or (n > lo_ and hi_ - lo_ > lim.click_margin):
                v = "INCONCLUSIVE"             # above every repeat, or within baselines that disagree
            else:
                v = "PASS"
            if cv is not None and cv < lim.periodic_cv:
                bper = any((_cv(b.ch[k]["clicks"], lim) or math.inf) < lim.periodic_cv for b in base)
                v = worst_verdict([v, "INCONCLUSIVE" if bper else "FAIL"])
            bloud = max([b.ch[k]["click_max_db"] or lim.click_db for b in base])
            if c["click_max_db"] is not None and c["click_max_db"] > bloud + lim.click_loud_db:
                v = "FAIL"                     # one event far louder than anything in the baselines
            row(rows, "(c) clicks (high band)", n, f"{lo_}..{hi_}", None,
                f"<= the baselines' most (scaled to length; above it INCONCLUSIVE, above + {lim.click_margin} FAIL; "
                f"baselines {lim.click_margin}+ apart: above their fewest INCONCLUSIVE); 1 ms frames or a matched "
                f"step +{lim.click_db:g} dB; interval CV >= {lim.periodic_cv:g}; none > the baselines' loudest "
                f"({bloud:g} dB) + {lim.click_loud_db:g} dB (loudest here {c['click_max_db']})"
                + (f" (CV {cv:.3f})" if cv is not None else ""), v == "PASS", k, band="high", verdict=v)
        if n and (is_base or base):
            rows[-1]["detail"] = c["clicks"][:20]
    sb = cap.split
    if is_base:
        row(rows, "(L) low-band bursts", f"{sb['excl_s']:g} s in {len(sb['bursts'])}", None, None,
            "baseline: the captures' allowance", True, band="lowx")
        if sb["bursts"]:
            rows[-1]["detail"] = [{k_: bu[k_] for k_ in ("t_s", "dur_s", "peak_db_re_quiet")} for bu in sb["bursts"]]
        return
    if not base:
        return
    low = [r for r in rows if r.get("band") == "low"]
    bad = sorted({r["metric"] for r in low if r["verdict"] != "PASS"})
    if not bad:
        row(rows, "(L) low band verdict", "PASS", None, None, "every (L) row passes on the whole window", True,
            band="lowv")
        return
    n, s = len(sb["bursts"]), sb["excl_s"]
    bn = max(len(b.split["bursts"]) for b in base)
    bs = max(b.split["excl_s"] for b in base)
    ex: list[tuple[str, bool]] = []
    ok = n <= bn and s <= bs + 1e-9
    row(rows, "(L) bursts vs baseline", f"{s:g} s in {n}", f"max {bs:g} s in {bn}", None,
        f"no more bursts, no more seconds than the baseline with the most (blocks > median +{lim.quiet_db:g} dB, "
        f"+-{lim.quiet_guard_blocks} guard)", ok, band="lowx", verdict=None if ok else "REVIEW")
    if sb["bursts"]:
        rows[-1]["detail"] = [{k_: bu[k_] for k_ in ("t_s", "dur_s", "peak_db_re_quiet", "dma_corr")}
                              for bu in sb["bursts"]][:20]
    ex.append(("bursts", ok))
    cv = _cv([bu["t_pk_s"] for bu in sb["bursts"]], lim)
    dma = max([bu["dma_corr"] for bu in sb["bursts"]], default=0.0)
    ok = (cv is None or cv >= lim.periodic_cv) and dma <= lim.quiet_dma_corr
    row(rows, "(L) burst rhythm", "-" if cv is None else rnd(cv, 3), None, rnd(dma, 3),
        f"interval CV >= {lim.periodic_cv:g}; 128 ms autocorrelation (delta) <= {lim.quiet_dma_corr:g}", ok,
        band="lowx", verdict=None if ok else "REVIEW")
    ex.append(("rhythm", ok))
    short = [x.label for x in [cap] + base if x.split["kept_s"] < lim.quiet_min_s]
    rq: list[dict] = []
    for k, c in enumerate(cap.ch):
        band_rows(rq, c["lo_q"], [b.ch[k]["lo_q"] for b in base], lim, k, "(L) ", "low")
    qbad = [{"metric": r["metric"], "ch": r["ch"], "verdict": r["verdict"], "delta": r["delta"], "value": r["value"],
             "baseline": r["baseline"]} for r in rq if r["verdict"] != "PASS"]
    nf = sum(q_["verdict"] == "FAIL" for q_ in qbad)
    ok = not nf and not short
    row(rows, "(L) recheck without the bursts", f"{nf} FAIL, {len(qbad) - nf} INCONCLUSIVE", None, None,
        f"no (L) row FAILS with the burst seconds left out (here and in the baselines, >= {lim.quiet_min_s:g} s "
        "left; INCONCLUSIVE - inside baselines that disagree - is accepted)"
        + (f"; too few seconds left in {', '.join(short)}" if short else ""), ok, band="lowx",
        verdict=None if ok else "REVIEW")
    if qbad:
        rows[-1]["detail"] = qbad[:20]
    ex.append(("recheck", ok))
    expl = all(o for _, o in ex)
    row(rows, "(L) low band verdict", "no (L) FAIL outside rumble within the baseline's" if expl else "not explained",
        None, None, "whole-window (L) rows not PASS: " + ", ".join(bad), expl, band="lowv",
        verdict="PASS" if expl else "REVIEW")


def compare(cap: Capture, base: list[Capture], lim: Limits, trim: tuple[float, float], dev: dict | None,
            expect_s: float | None, dev_state: str = "none", ref_devs: list[dict] | None = None,
            dev_how: str = "", is_base: bool = False) -> list[dict]:
    """dev_state: "none" (no --bench-log), "found", "missing" (no line for this file) or "waived".
    ref_devs: the baseline captures' counters (the other baselines when cap is one). dev_how: how the counter line
    was matched (or which names were tried)."""
    rows: list[dict] = []
    ref_devs = ref_devs or []
    if base and len(base[0].ch) != len(cap.ch):
        raise ValueError(f"{cap.path}: {len(cap.ch)} channel(s), baseline has {len(base[0].ch)}")
    if base and any((b.split is None) != (cap.split is None) for b in base):
        raise ValueError(f"{cap.path}: --band-split must apply to the baseline and the captures alike")
    for k, c in enumerate(cap.ch):
        bk = [b.ch[k] for b in base]
        if bk and any(b["signal"] != c["signal"] for b in bk):
            raise ValueError(f"{cap.path} ch{k}: signal {c['signal']}, baseline differs (same signal needed)")
        if cap.split is None:
            band_rows(rows, c, bk, lim, k)
        else:
            band_rows(rows, c["hi"], [b["hi"] for b in bk], lim, k, "", "high")
            band_rows(rows, c["lo"], [b["lo"] for b in bk], lim, k, "(L) ", "low")
        # (e) phase
        if c["signal"] == "tone":
            st = c["phase_steps"]
            row(rows, "(e) tone phase steps", len(st), 0, None, f"none > {lim.phase_rad} rad", not st, k)
            if st:
                rows[-1]["detail"] = st
    if cap.split is not None:
        split_rows(rows, cap, base, lim, is_base)
    # (e) zero runs and length: the whole file but the edges
    e = min(int(lim.cont_edge_s * FS), cap.n // 4)
    zr = zero_runs(cap.raw[e:cap.n - e], lim.zero_run)
    for z in zr:
        z["t_s"] = round(z["t_s"] + e / FS, 3)
    row(rows, "(e) zero runs", len(zr), 0, None, f"none >= {lim.zero_run} samples", not zr)
    if zr:
        rows[-1]["detail"] = zr[:20]
    n = cap.n
    if dev_state == "missing":
        row(rows, "(e) device counters", "missing", None, None,
            "a counter line for this file (ring wrapped? renamed? map it: --labels, LABEL=path@REC_NNNN.wav, "
            "--bench-log LABEL=log) or --no-device-counters LABEL", False)
        if dev_how:
            rows[-1]["detail"] = [dev_how]
    elif dev_state == "waived":
        row(rows, "(e) device counters", "waived", None, None, "--no-device-counters: audio metrics only", True)
    if dev is not None:
        row(rows, "(e) device record", dev.get("file", "?"), None, None, f"matched by {dev_how or 'file name'}", True)
        clk = int(dev.get("clk_smp", "0") or 0)
        row(rows, "(e) length vs device clock", n, clk, n - clk, f"+-{lim.len_tol}", abs(n - clk) <= lim.len_tol)
        wav = int(dev.get("wav_smp", "-1") or -1)
        row(rows, "(e) samples = device wav_smp", n, wav, n - wav, "exact", n == wav)
        for key in ("ov", "lost", "acct"):
            v = dev.get(key)
            row(rows, f"(e) device {key}", v, 0, None, "0", v == "0")
        if dev.get("vor", "0") != "0":
            rows[-1]["note"] = "VOR on: acct is not meaningful"
        # block clock: sub-block loss (the DMA did not capture samples) to a few samples
        cb = _bdrift(dev)
        refs = [r for r in (_bdrift(d) for d in ref_devs) if r is not None]
        if cb is None:
            row(rows, "(e) block-clock drift", dev.get("bdrift", "absent"), None, None,
                "bdrift / bclk_smp needed (this firmware or record has none)", False, verdict="INCONCLUSIVE")
        elif refs:
            bd, bc = cb
            ppm = bd / bc * 1e6
            rates = [r[0] / r[1] for r in refs]
            lo, hi = min(rates) * bc, max(rates) * bc
            exc = bd - min(max(bd, lo), hi)
            v = None
            if abs(exc) <= lim.drift_smp and hi - lo > 2 * lim.drift_smp:
                v = "INCONCLUSIVE"                  # the C0 block clocks disagree: the bar means nothing
            row(rows, "(e) block-clock drift vs C0", bd, f"{lo:.0f}..{hi:.0f}", rnd(exc, 1),
                f"+-{lim.drift_smp} smp outside the C0 range", abs(exc) <= lim.drift_smp, verdict=v)
            row(rows, "(e) block-clock rate", bd, None, rnd(ppm, 1),
                f"|ppm| <= {lim.drift_ppm:g}", abs(ppm) <= lim.drift_ppm)
        else:
            bd, bc = cb
            ppm = bd / bc * 1e6
            row(rows, "(e) block-clock drift", bd, None, rnd(ppm, 1),
                f"|ppm| <= {lim.drift_ppm:g} (no C0 counters: sub-block loss not resolved)",
                abs(ppm) <= lim.drift_ppm)
        # settings: the same as the baseline's
        mine = tuple(dev.get(k_) for k_ in SETUP_KEYS)
        theirs = {tuple(d.get(k_) for k_ in SETUP_KEYS) for d in ref_devs}
        if theirs:
            ok = theirs == {mine}
            row(rows, "(setup) gain/dsp/vor/ch", "/".join(str(v) for v in mine),
                ", ".join("/".join(str(v) for v in t) for t in sorted(theirs, key=str)), None, "= baseline", ok)
    elif expect_s is not None:
        ex = int(round(expect_s * FS))
        row(rows, "(e) length vs --expect-seconds", n, ex, n - ex, f"+-{lim.len_tol}", abs(n - ex) <= lim.len_tol)
    elif dev_state == "none":
        row(rows, "(e) length", n, None, None, "not checked (no --bench-log / --expect-seconds)", True)
    return rows


# ----------------------------------------------------------------------------------------------------- report
def dev_key(name: str) -> str:
    """A device file name as the counter lines key it: no directory, upper case, .WAV added when missing."""
    k = Path(name.strip().replace("\\", "/")).name.upper()
    return k if k.endswith(".WAV") else k + ".WAV"


def parse_labels(paths: list[Path]) -> dict[str, str]:
    """LABEL -> device file from lines "LABEL REC_0032.wav" (also "LABEL=..." / "LABEL,..."; '#' comments)."""
    out: dict[str, str] = {}
    for p in paths:
        for line in p.read_text(errors="replace").splitlines():
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = [t for t in re.split(r"[\s,=;]+", line) if t]
            if len(parts) < 2:
                raise ValueError(f"{p}: cannot read label line {line!r} (want: LABEL REC_NNNN.wav)")
            out[parts[0]] = parts[-1]
    return out


def match_device(label: str, p: Path, dev: dict[str, dict[str, str]], label_logs: dict[str, dict[str, dict]],
                 dev_names: dict[str, str]) -> tuple[dict | None, str]:
    """The counter line for a capture, and how it was matched (or what was tried). Order: the explicit device name
    (LABEL=path@REC_x.wav or --labels), then the WAV's own name; the capture's own log (--bench-log LABEL=log) first,
    then all logs; a capture's own log with a single finished record serves it when no explicit name is given."""
    explicit = dev_names.get(label)
    names = [dev_key(explicit)] if explicit else []
    if dev_key(p.name) not in names and not explicit:
        names.append(dev_key(p.name))
    own = label_logs.get(label)
    if own is not None:
        for nm in names:
            if nm in own:
                return own[nm], f"{nm} in {label}'s log" + (" (explicit name)" if explicit else "")
    for nm in names:
        if nm in dev:
            return dev[nm], nm + (" (explicit name)" if explicit else " (WAV name)")
    if own is not None and not explicit:
        fin = [k for k, v in own.items() if v.get("rec") == "0"] or list(own)
        if len(fin) == 1:
            return own[fin[0]], f"{fin[0]}: the only finished record in {label}'s log"
    tried = ", ".join(names) + (f"; {label}'s log holds {sorted(own)}" if own is not None else "")
    return None, "tried " + tried


def run(base_paths: list[tuple[str, Path]], caps: list[tuple[str, Path]], signal: str, tone_hz: float | None,
        trim: tuple[float, float], bench_logs: list[Path], expect_s: float | None, lim: Limits,
        waive: set[str] | None = None, split: bool = False, dev_names: dict[str, str] | None = None,
        label_logs: dict[str, list[Path]] | None = None) -> dict:
    """dev_names: capture label -> device file name (REC_0032.wav); label_logs: label -> that capture's own logs
    (also searched for every other capture)."""
    waive = waive or set()
    dev_names = dev_names or {}
    label_logs = label_logs or {}
    all_logs = list(bench_logs) + [q for ps in label_logs.values() for q in ps if q not in bench_logs]
    dev = parse_bench_log(all_logs) if all_logs else {}
    own = {lb: parse_bench_log(ps) for lb, ps in label_logs.items()}

    def lookup(label: str, p: Path) -> tuple[dict | None, str, str]:
        if not all_logs:
            return None, "none", ""
        d, how = match_device(label, p, dev, own, dev_names)
        if d is not None:
            return d, "found", how
        return None, "waived" if label in waive else "missing", how

    base = [load_capture(lb, p, signal, tone_hz, trim, lim, split) for lb, p in base_paths]
    bdev = [lookup(lb, p) for lb, p in base_paths]
    report = {"baseline": [str(p) for _, p in base_paths], "limits": lim.__dict__, "band_split": split,
              "captures": []}
    for i, b in enumerate(base):   # the baseline itself must be continuous; its counters vs the other repeats
        others = [d for j, (d, _, _) in enumerate(bdev) if j != i and d is not None]
        rows = compare(b, [], lim, trim, bdev[i][0], expect_s, bdev[i][1], others, bdev[i][2], is_base=True)
        report["captures"].append(summary(b, rows, is_base=True))
    refs = [d for d, _, _ in bdev if d is not None]
    for lb, p in caps:
        c = load_capture(lb, p, signal, tone_hz, trim, lim, split)
        d, st, how = lookup(lb, p)
        rows = compare(c, base, lim, trim, d, expect_s, st, refs, how)
        report["captures"].append(summary(c, rows, is_base=False))
    report["pass"] = all(c["pass"] for c in report["captures"])
    return report


def _public(d):
    """A report-safe copy: no "_" keys, no masks, no sample indices."""
    if isinstance(d, dict):
        return {k: _public(v) for k, v in d.items()
                if not k.startswith("_") and k not in ("mask", "s", "e", "pk")}
    if isinstance(d, list):
        return [_public(v) for v in d]
    return d


def summary(c: Capture, rows: list[dict], is_base: bool) -> dict:
    main_rows = [r for r in rows if r.get("band") not in LOW_BANDS]
    verdict = worst_verdict(r["verdict"] for r in main_rows)
    low = next((r["verdict"] for r in rows if r.get("band") == "lowv"), None)
    if verdict == "PASS" and low == "REVIEW":
        verdict = "REVIEW"
    hi_v = worst_verdict(r["verdict"] for r in rows if r.get("band") == "high") if c.split else None
    return {"label": c.label, "file": str(c.path), "baseline": is_base, "samples": c.n, "channels": len(c.ch),
            "seconds": round(c.n / FS, 3), "per_channel": _public(c.ch),
            "band_split": _public(c.split) if c.split is not None else None,
            "high_band_verdict": hi_v, "low_band_verdict": low, "rows": rows, "verdict": verdict,
            "pass": verdict == "PASS"}


def _sig(ch: dict) -> str:
    if ch["tone_hz"]:
        t = ch.get("hi") if ch.get("hi", {}).get("tone_hz") else ch.get("lo", ch)
        return f" {ch['tone_hz']} Hz at {t.get('tone_dbfs')} dBFS"
    if "hi" in ch:
        return (f" floor >= split {ch['hi'].get('floor_db')} dBFS ({ch['hi'].get('floor_a_db')} A), "
                f"< split {ch['lo'].get('floor_db')} dBFS")
    return f" floor {ch.get('floor_db')} dBFS ({ch.get('floor_a_db')} A)" if "floor_db" in ch else ""


def markdown(rep: dict) -> str:
    out = ["# RF audio check", "", f"Baseline: {', '.join(rep['baseline'])}", "",
           f"**Overall: {'PASS' if rep['pass'] else 'NOT PASS'}**", ""]
    if rep.get("band_split"):
        out += ["Band split at " + f"{rep['limits']['split_hz']:g} Hz: high band, continuity and clicks on the whole "
                "window decide FAIL / INCONCLUSIVE; the low band (mechanical rumble region, rows (L)) can only make "
                "a capture REVIEW.", ""]
    for c in rep["captures"]:
        kind = "baseline (continuity, counters)" if c["baseline"] else "capture"
        sig = ", ".join(f"ch{i} {ch['signal']}" + _sig(ch) for i, ch in enumerate(c["per_channel"]))
        bs = ""
        if c.get("band_split"):
            q = c["band_split"]
            at = ", ".join(f"{bu['t_s']:g}" for bu in q["bursts"])
            bs = f"; low-band bursts: {q['excl_s']:g} s in {len(q['bursts'])}" + (f" (at {at} s)" if at else "")
            if q.get("hb_loud_s"):
                bs += (f"; high-band seconds > median +6 dB at {', '.join(f'{t:g}' for t in q['hb_loud_s'][:12])} s, "
                       f"{q['hb_excess_in_bursts']} of the high-band excess energy in low-band burst seconds "
                       "(information only)")
            if not c["baseline"]:
                bs += f"\n\nHigh band + continuity: **{c['high_band_verdict']}**; low band (mechanical rumble " \
                      f"region): **{c['low_band_verdict']}**"
        out += [f"## {c['label']} - {c['verdict']}", "",
                f"{kind}: `{c['file']}`, {c['seconds']} s, {c['channels']} ch, {sig}{bs}", "",
                "| metric | band | ch | value | baseline | delta | limit | verdict |",
                "|---|---|---|---|---|---|---|---|"]
        for r in c["rows"]:
            ch = "" if r["ch"] is None else r["ch"]
            bd = {"high": "high", "low": "low", "lowx": "low (why)", "lowv": "low"}.get(r.get("band"), "")
            out.append(f"| {r['metric']} | {bd} | {ch} | {r['value']} | {r['baseline']} | "
                       f"{'' if r['delta'] is None else r['delta']} | {r['limit']} | **{r['verdict']}** |")
        for r in c["rows"]:
            if r.get("detail"):
                out.append(f"\n- {r['metric']}: `{json.dumps(r['detail'][:8])}`")
        out.append("")
    return "\n".join(out)


def parse_labeled(s: str) -> tuple[str, Path]:
    lb, p, _ = parse_capture_arg(s)
    return lb, p


def parse_capture_arg(s: str) -> tuple[str, Path, str | None]:
    """[LABEL=]path[@DEVICE.wav] -> (label, path, device file name or None). The @ suffix is only taken when the
    whole string is not an existing file and the suffix is a bare *.wav name."""
    dev = None
    if not Path(s).exists() and "@" in s:
        head, tail = s.rsplit("@", 1)
        if re.fullmatch(r"[^/\\=@]+\.wav", tail, flags=re.I):
            s, dev = head, tail
    if "=" in s and not Path(s).exists():
        lb, p = s.split("=", 1)
        return lb, Path(p), dev
    return Path(s).stem, Path(s), dev


# ----------------------------------------------------------------------------------------------------- self-test
def _band_noise(rng: np.random.Generator, n: int, lo: float, hi: float, dbfs: float) -> np.ndarray:
    """Gaussian noise band-limited to lo..hi Hz at dbfs RMS."""
    spec = np.fft.rfft(rng.standard_normal(n))
    f = np.fft.rfftfreq(n, 1.0 / FS)
    spec[(f < lo) | (f > hi)] = 0.0
    x = np.fft.irfft(spec, n)
    return x / max(float(np.sqrt(np.mean(x ** 2))), 1e-30) * 10 ** (dbfs / 20.0)


def _synth(seconds: float, seed: int, tone_hz: float | None = None, ch: int = 1, floor_dbfs: float = -70.0,
           spur: tuple[float, float] | None = None, am: tuple[float, float] | None = None,
           tone_db: float = 0.0, h3_dbc: float | None = None, wander: tuple[float, float] | None = None,
           bursts: tuple[float, float, float] | None = None, lf_dbfs: float | None = None,
           band: tuple[float, float, float] | None = None) -> np.ndarray:
    """Test signal: white floor + 50 Hz hum, optionally a tone (amplitude 0.1 * tone_db, AM (rate, depth), FM wander
    (peak Hz, rate Hz), 3rd harmonic), a spur (Hz, dBFS), noise bursts (period s, length ms, dB over the floor),
    25-60 Hz noise (dBFS) and band noise (lo, hi, dBFS)."""
    rng = np.random.default_rng(seed)
    n = int(seconds * FS)
    t = np.arange(n) / FS
    out = np.empty((n, ch))
    for k in range(ch):
        x = rng.standard_normal(n) * 10 ** (floor_dbfs / 20.0)
        x += 10 ** (-80 / 20.0) * math.sqrt(2) * np.sin(2 * np.pi * 50.0 * t + k)        # mains hum, every capture
        if tone_hz:
            g = 0.1 * 10 ** (tone_db / 20.0) * (1.0 + (am[1] * np.sin(2 * np.pi * am[0] * t) if am else 0.0))
            ph = 2 * np.pi * tone_hz * t + 0.3 * k
            if wander:
                ph = ph + wander[0] / wander[1] * np.sin(2 * np.pi * wander[1] * t)
            x += g * np.sin(ph)                                                           # -23 dBFS RMS
            if h3_dbc is not None:
                x += g * 10 ** (h3_dbc / 20.0) * np.sin(3 * ph)
        if spur:
            x += 10 ** (spur[1] / 20.0) * math.sqrt(2) * np.sin(2 * np.pi * spur[0] * t)
        if lf_dbfs is not None:
            x += _band_noise(rng, n, 25.0, 60.0, lf_dbfs)
        if band:
            x += _band_noise(rng, n, band[0], band[1], band[2])
        if bursts:
            m = int(bursts[1] * FS / 1000)
            t0 = 0.3
            while int(t0 * FS) + m < n:
                s = int(t0 * FS)
                x[s:s + m] += rng.standard_normal(m) * 10 ** ((floor_dbfs + bursts[2]) / 20.0)
                t0 += bursts[0]
        out[:, k] = x
    return out


def _dev_line(name: str, n: int, bdrift: int | None = 0, gain: int = 20, rec: int = 0, ov: int = 0,
              wav: int | None = None, clk_off: int = -1000, sep: str = " ") -> str:
    kv = [("seq", 1), ("file", name), ("rec", rec), ("ch", 1), ("gain", gain), ("dsp", 2), ("vor", 0),
          ("t_ms", n // 16), ("dma_smp", n), ("clk_smp", n - clk_off), ("drift", clk_off),
          ("wav_smp", n if wav is None else wav), ("ov", ov), ("comp", 0), ("lost", ov), ("acct", 0)]
    if bdrift is not None:
        kv += [("bclk_smp", n - 4096 - bdrift), ("bdrift", bdrift)]
    kv += [("sd_err", 0), ("sd_mdto", 0)]
    return sep.join(f"{k}={v}" for k, v in kv)


def self_test() -> int:
    lim = Limits()
    sec, trim = 75.0, (7.5, 7.5)            # the recipe's capture and window
    results: list[tuple[str, bool, str]] = []

    def expect(name: str, rep: dict, label: str, want: str, want_metrics: tuple[str, ...] = (),
               not_metrics: tuple[str, ...] = ()) -> None:
        """want: the capture's verdict; want_metrics: rows that must not PASS; not_metrics: rows that must PASS."""
        c = next(x for x in rep["captures"] if x["label"] == label)
        bad_rows = sorted({r["metric"] for r in c["rows"] if r["verdict"] != "PASS"})
        ok = c["verdict"] == want and all(any(m in f for f in bad_rows) for m in want_metrics)
        ok = ok and not any(any(m in f for f in bad_rows) for m in not_metrics)
        results.append((name, ok, f"verdict={c['verdict']} not-pass={bad_rows}"))

    with tempfile.TemporaryDirectory(prefix="rf_audio_check_") as td:
        d = Path(td)

        def save(name: str, x: np.ndarray) -> tuple[str, Path]:
            save_wav(d / f"{name}.wav", x)
            return name, d / f"{name}.wav"

        # ---- silence
        b = [save("C0a", _synth(sec, 1)), save("C0b", _synth(sec, 2))]
        drop = _synth(sec, 9)
        drop[10 * FS:10 * FS + BLOCK] = 0.0
        edge = _synth(sec, 10)
        edge[int(2.0 * FS):int(2.0 * FS) + BLOCK] = 0.0          # inside the 7.5 s trim: continuity still sees it
        caps = [save("clean", _synth(sec, 3)), save("spur", _synth(sec, 4, spur=(1600.0, -100.0))),
                save("floor", _synth(sec, 5, floor_dbfs=-69.0)), save("drop", drop), save("edge", edge),
                save("peak", _synth(sec, 6, spur=(3333.0, -98.0))),
                save("b1hz", _synth(sec, 7, bursts=(1.0, 20.0, 6.0))),
                save("b12hz", _synth(sec, 8, bursts=(0.078125, 20.0, 6.0))),
                save("b128", _synth(sec, 11, bursts=(1.28, 11.25, 6.0)))]
        rep = run(b, caps, "auto", None, trim, [], None, lim)
        expect("silence baseline continuity", rep, "C0a", "PASS")
        expect("silence clean vs C0", rep, "clean", "PASS")
        expect("1600 Hz spur detected", rep, "spur", "FAIL", ("(b) spur targets",), ("(a) noise floor",))
        expect("+1 dB noise floor detected", rep, "floor", "FAIL", ("(a) noise floor", "(a) noise floor A-wtd"))
        expect("2048-sample dropout detected", rep, "drop", "FAIL", ("(e) zero runs",))
        expect("dropout 2 s in (inside the spectral trim) detected", rep, "edge", "FAIL", ("(e) zero runs",))
        expect("off-target narrow line (3333 Hz) detected", rep, "peak", "FAIL", ("(b) new narrow peaks",),
               ("(b) spur targets",))
        expect("20 ms +6 dB bursts every 1 s -> envelope line", rep, "b1hz", "FAIL", ("(c) envelope lines",))
        expect("20 ms +6 dB bursts every 78 ms (12.8 Hz GETS) -> envelope line", rep, "b12hz", "FAIL",
               ("(c) envelope lines",))
        expect("11 ms +6 dB bursts every 1.28 s -> fold + 0.78 Hz", rep, "b128", "FAIL",
               ("(c) 1.28 s fold depth", "(c) envelope 0.78125 Hz"))
        # A-weighted floor on its own: 25-60 Hz noise dominates the unweighted floor, 1-4 kHz noise raises only A
        b_lf = [save("L0a", _synth(sec, 21, lf_dbfs=-55.0)), save("L0b", _synth(sec, 22, lf_dbfs=-55.0))]
        rep = run(b_lf, [save("lfa", _synth(sec, 23, lf_dbfs=-55.0, band=(1000.0, 4000.0, -75.0)))], "auto", None,
                  trim, [], None, lim)
        expect("A-weighted floor alone (+1 dB at 1-4 kHz)", rep, "lfa", "FAIL", ("(a) noise floor A-wtd",))
        ra = next(r for r in rep["captures"][-1]["rows"] if r["metric"] == "(a) noise floor")
        results.append(("... the unweighted floor stays within 0.5 dB", ra["verdict"] == "PASS", f"delta {ra['delta']}"))
        # a noisy baseline cannot support the bar: INCONCLUSIVE, not PASS
        b_noisy = [save("N0a", _synth(sec, 24, floor_dbfs=-70.75)), save("N0b", _synth(sec, 25, floor_dbfs=-69.25))]
        rep = run(b_noisy, [save("nmid", _synth(sec, 26))], "auto", None, trim, [], None, lim)
        expect("baselines 1.5 dB apart -> INCONCLUSIVE", rep, "nmid", "INCONCLUSIVE",
               ("(a) noise floor", "(b) baseline repeatability"))
        rep = run(b[:1], [("clean", d / "clean.wav")], "auto", None, trim, [], None, lim)
        expect("a single baseline -> INCONCLUSIVE", rep, "clean", "INCONCLUSIVE", ("(b) baseline repeatability",))
        # stereo
        rep = run([save("S0a", _synth(sec, 27, ch=2)), save("S0b", _synth(sec, 28, ch=2))],
                  [save("sspur", _synth(sec, 29, ch=2, spur=(266.67, -98.0)))], "auto", None, trim, [], None, lim)
        expect("stereo: 266.7 Hz spur in both channels", rep, "sspur", "FAIL", ("(b) spur targets",))

        # ---- tone (997 Hz)
        tone = 997.0
        bt = [save("T0a", _synth(sec, 31, tone)), save("T0b", _synth(sec, 32, tone))]
        caps = [save("tclean", _synth(sec, 33, tone)),
                save("tam", _synth(sec, 34, tone, am=(0.78125, 0.003))),
                save("tam2", _synth(sec, 35, tone, am=(2.34375, 0.003))),
                save("tam7", _synth(sec, 36, tone, am=(7.5, 0.003))),
                save("tnoise", _synth(sec, 37, tone, floor_dbfs=-67.0)),
                save("thd", _synth(sec, 38, tone, h3_dbc=-50.0)),
                save("twander", _synth(sec, 39, tone, wander=(0.02, 0.01))),
                save("twnoise", _synth(sec, 40, tone, wander=(0.02, 0.01), floor_dbfs=-67.0)),
                save("tlevel", _synth(sec, 41, tone, tone_db=0.5, floor_dbfs=-69.1)),
                save("toff", _synth(sec, 42, tone + 0.02))]
        x = _synth(sec, 43, tone)
        caps.append(save("tstep", np.delete(x, 30 * FS, axis=0)))
        x = _synth(sec, 44, tone)
        caps.append(save("tloss", np.delete(x, np.s_[30 * FS:30 * FS + BLOCK], axis=0)))
        x = _synth(sec, 45, tone)
        x[30 * FS:30 * FS + BLOCK] = 0.0
        caps.append(save("tzero", x))
        for m in (48, 100):                      # sub-block losses that a 997 Hz phase step can see, mid-block
            x = _synth(sec, 46 + m, tone)
            caps.append(save(f"tl{m}", np.delete(x, np.s_[30 * FS + 128:30 * FS + 128 + m], axis=0)))
        x = _synth(sec, 47, tone)
        caps.append(save("tedge", np.delete(x, int(3.0 * FS), axis=0)))   # 3 s in: inside the spectral trim
        rep = run(bt, caps, "auto", None, trim, [], sec, lim)
        expect("tone baseline continuity + length", rep, "T0a", "PASS")
        expect("tone clean vs C0", rep, "tclean", "PASS")
        expect("0.78 Hz AM (0.3 %) detected", rep, "tam", "FAIL", ("(c) envelope 0.78125 Hz",))
        expect("2.34 Hz AM (0.3 %) detected", rep, "tam2", "FAIL", ("(c) envelope 2.34375 Hz",))
        expect("7.5 Hz AM (0.3 %) detected", rep, "tam7", "FAIL", ("(c) envelope 7.5 Hz",))
        expect("+3 dB noise under the tone -> SNR, residual noise", rep, "tnoise", "FAIL",
               ("(d) SNR", "(d) residual noise dBFS"))
        expect("3rd harmonic -50 dBc -> THD+N", rep, "thd", "FAIL", ("(d) THD+N",), ("(d) SNR",))
        expect("tone wandering 0.02 Hz is not noise", rep, "twander", "PASS")
        expect("tone 0.02 Hz off (auto-detected) is not noise", rep, "toff", "PASS")
        expect("+3 dB noise under a wandering tone -> SNR", rep, "twnoise", "FAIL", ("(d) SNR",))
        expect("tone +0.5 dB with noise +0.9 dB -> residual noise, level", rep, "tlevel", "FAIL",
               ("(d) residual noise dBFS", "(d) tone level"))
        expect("1-sample loss -> phase step", rep, "tstep", "FAIL", ("(e) tone phase steps",))
        expect("2048-sample loss -> phase step (one block is within the length tolerance)", rep, "tloss", "FAIL",
               ("(e) tone phase steps",))
        expect("zero block in a tone", rep, "tzero", "FAIL", ("(e) zero runs",))
        expect("48-sample loss mid-block -> split phase step", rep, "tl48", "FAIL", ("(e) tone phase steps",))
        expect("100-sample loss mid-block -> phase step", rep, "tl100", "FAIL", ("(e) tone phase steps",))
        expect("1-sample loss 3 s in (inside the spectral trim) -> phase step", rep, "tedge", "FAIL",
               ("(e) tone phase steps",))
        tc = next(x for x in rep["captures"] if x["label"] == "tclean")
        f0 = tc["per_channel"][0]["tone_hz"]
        results.append(("tone frequency estimate", abs(f0 - tone) < 0.05, f"{f0} Hz"))

        # ---- device counters (--bench-log)
        x = _synth(sec, 51)
        n = len(x)
        names = {"c0a": "REC_0001", "c0b": "REC_0002", "ok": "REC_0003", "bad": "REC_0004", "l16": "REC_0005",
                 "l32": "REC_0006", "miss": "REC_0007", "waive": "REC_0008", "prio": "REC_0009", "gain": "REC_0010",
                 "old": "REC_0011"}
        for nm in names.values():
            save_wav(d / f"{nm}.wav", x)
        ring = [
            "BENCH rec " + _dev_line("REC_0001.wav", n, bdrift=-2),
            "+XREC:DBG,REC,0," + _dev_line("REC_0002.wav", n, bdrift=1, sep=","),
            "noise from another task BENCH rec " + _dev_line("REC_0003.wav", n, bdrift=-1),
            "BENCH rec " + _dev_line("REC_0004.wav", n, ov=1, wav=n - 2048, clk_off=-5000),
            "BENCH final rec i=0 " + _dev_line("REC_0005.wav", n, bdrift=-18),
            "BENCH rec " + _dev_line("REC_0006.wav", n, bdrift=-33),
            "BENCH rec " + _dev_line("REC_0009.wav", n, bdrift=0),
            "BENCH rec " + _dev_line("REC_0010.wav", n, bdrift=0, gain=31),
            "BENCH rec " + _dev_line("REC_0011.wav", n, bdrift=None),
        ]
        (d / "ring.log").write_text("\n".join(ring) + "\n")
        # a later file with a mid-recording line for REC_0009 (rec=1, short): the final record must still win
        (d / "dbg.log").write_text("+XREC:DBG,LIVE," + _dev_line("REC_0009.wav", n // 2, bdrift=-400, rec=1, sep=",")
                                   + ",gets_tot=0\r\n")
        bd = [("C0a", d / "REC_0001.wav"), ("C0b", d / "REC_0002.wav")]
        caps = [(k, d / f"{v}.wav") for k, v in names.items() if not k.startswith("c0")]
        rep = run(bd, caps, "auto", None, trim, [d / "ring.log", d / "dbg.log"], None, lim, waive={"waive"})
        expect("device counters: baseline", rep, "C0a", "PASS")
        expect("device counters clean (bdrift inside the C0 range)", rep, "ok", "PASS")
        expect("device counters: overrun, length, wav_smp", rep, "bad", "FAIL",
               ("(e) device ov", "(e) length vs device clock", "(e) samples = device wav_smp", "(e) device lost"))
        expect("16-sample sub-block loss -> block clock", rep, "l16", "FAIL", ("(e) block-clock drift vs C0",))
        expect("32-sample sub-block loss -> block clock", rep, "l32", "FAIL", ("(e) block-clock drift vs C0",))
        expect("no counter line for a capture -> FAIL", rep, "miss", "FAIL", ("(e) device counters",))
        expect("--no-device-counters waives it", rep, "waive", "PASS")
        expect("final record beats a later mid-recording line", rep, "prio", "PASS")
        expect("gain differs from the baseline -> setup", rep, "gain", "FAIL", ("(setup) gain/dsp/vor/ch",))
        expect("record without a block clock -> INCONCLUSIVE", rep, "old", "INCONCLUSIVE", ("(e) block-clock drift",))
        # systematic gross block-clock rate error across BOTH C0 baselines and capture (> 300 ppm) must FAIL even when
        # C0a, C0b and the capture agree with each other within +-8 samples
        ring_sys = [
            "BENCH rec " + _dev_line("REC_0001.wav", n, bdrift=-1000),
            "BENCH rec " + _dev_line("REC_0002.wav", n, bdrift=-1002),
            "BENCH rec " + _dev_line("REC_0003.wav", n, bdrift=-1001),
        ]
        (d / "ring_sys.log").write_text("\n".join(ring_sys) + "\n")
        rep_sys = run(bd, [("ok", d / "REC_0003.wav")], "auto", None, trim, [d / "ring_sys.log"], None, lim)
        expect("systematic -837 ppm block-clock drift on C0 + capture -> FAIL", rep_sys, "C0a", "FAIL",
               ("(e) block-clock",))
        expect("systematic -837 ppm block-clock drift on capture -> FAIL", rep_sys, "ok", "FAIL",
               ("(e) block-clock",))
        md = markdown(rep)
        results.append(("markdown table", "| (e) device ov |" in md and "**FAIL**" in md, f"{len(md)} chars"))
        json.dumps(rep)

        # ---- device counters matched by label (captures saved as LABEL.wav, not REC_NNNN.wav), via the CLI
        for nm in ("M0a", "M0b", "M1", "M2", "M3", "M4", "M5"):
            save_wav(d / f"{nm}.wav", x)
        (d / "m_ring.log").write_text("\n".join([
            "BENCH recrun start t=5", "BENCH rec " + _dev_line("REC_0101.wav", n, bdrift=-1),
            "BENCH rec " + _dev_line("REC_0102.wav", n, bdrift=0),
            "BENCH rec " + _dev_line("REC_0103.wav", n, bdrift=-1),
            "BENCH rec " + _dev_line("REC_0104.wav", n, bdrift=0),
            "BENCH rec " + _dev_line("REC_0106.wav", n, bdrift=0, ov=1)]) + "\n")
        (d / "m3.log").write_text("BENCH rec " + _dev_line("REC_0105.wav", n // 2, bdrift=0, rec=1) + "\n"
                                  "BENCH rec " + _dev_line("REC_0105.wav", n, bdrift=-1) + "\n")
        (d / "m4.log").write_text("BENCH rec " + _dev_line("REC_0107.wav", n, bdrift=0) + "\n"
                                  "BENCH rec " + _dev_line("REC_0108.wav", n, bdrift=0) + "\n")
        (d / "labels.txt").write_text("# label device-file\nM0a REC_0101.wav\nM0b=REC_0102.wav\nM1 REC_0103.wav\n"
                                      "M5 REC_0106.wav\n")
        argv = ["--labels", str(d / "labels.txt"), "--bench-log", str(d / "m_ring.log"),
                "--baseline", f"M0a={d / 'M0a.wav'}", "--baseline", f"M0b={d / 'M0b.wav'}",
                "--capture", f"M1={d / 'M1.wav'}", "--capture", f"M2={d / 'M2.wav'}@REC_0104.wav",
                "--capture", f"M3={d / 'M3.wav'}", "--bench-log", f"M3={d / 'm3.log'}",
                "--capture", f"M4={d / 'M4.wav'}", "--bench-log", f"M4={d / 'm4.log'}",
                "--capture", f"M5={d / 'M5.wav'}", "--trim-start-s", "7.5", "--trim-end-s", "7.5",
                "--json", str(d / "m.json")]
        with contextlib.redirect_stdout(io.StringIO()):
            rc = main(argv)
        rep = json.loads((d / "m.json").read_text())
        results.append(("label mapping: CLI exit status 1 (M4, M5 fail)", rc == 1, f"rc={rc}"))
        expect("labels.txt maps a baseline to its device record", rep, "M0a", "PASS")
        expect("labels.txt maps a capture (LABEL.wav) to REC_0103", rep, "M1", "PASS")
        expect("LABEL=path@REC_0104.wav names the device file", rep, "M2", "PASS")
        expect("--bench-log LABEL=log: its only finished record (rec=1 line ignored)", rep, "M3", "PASS")
        expect("--bench-log LABEL=log with two records and no name -> missing", rep, "M4", "FAIL",
               ("(e) device counters",))
        expect("labels.txt match uses that record's counters (ov=1)", rep, "M5", "FAIL", ("(e) device ov",))
        m1 = next(c for c in rep["captures"] if c["label"] == "M1")
        results.append(("... the matched record is named in the report",
                        any(r["metric"] == "(e) device record" and r["value"] == "REC_0103.wav" for r in m1["rows"]),
                        "REC_0103.wav"))
        pc = parse_capture_arg("C1_1=caps/C1_1.wav@REC_0033.wav")
        results.append(("parse LABEL=path@DEVICE.wav", pc == ("C1_1", Path("caps/C1_1.wav"), "REC_0033.wav"),
                        str(pc)))

        # ---- --band-split: mechanical rumble bursts (40-150 Hz, near full scale) in the baseline and the captures
        def rumble(x: np.ndarray, times: list[float], seed: int, lvl: float = -10.0, dur_ms: float = 300.0,
                   comb_dbfs: float | None = None) -> np.ndarray:
            """Hann-shaped 40-150 Hz noise bursts; comb_dbfs adds 800/1600/2400 Hz lines inside each burst."""
            rng = np.random.default_rng(seed)
            m = int(dur_ms * FS / 1000)
            w = np.hanning(m)
            for t0 in times:
                s0 = int(t0 * FS)
                b_ = _band_noise(rng, m, 40.0, 150.0, lvl) * w
                if comb_dbfs is not None:
                    tt = np.arange(m) / FS
                    b_ = b_ + sum(10 ** (comb_dbfs / 20.0) * math.sqrt(2) * np.sin(2 * np.pi * fk * tt)
                                  for fk in (800.0, 1600.0, 2400.0))
                x[s0:s0 + m, :] += b_[:, None]
            return x

        def add(x: np.ndarray, t0: float, y: np.ndarray) -> np.ndarray:
            x[int(t0 * FS):int(t0 * FS) + len(y), :] += y[:, None]
            return x

        def tones(freqs: list[float], dur_s: float, dbfs: float) -> np.ndarray:
            """A sum of sines, each at dbfs RMS."""
            tt = np.arange(int(dur_s * FS)) / FS
            return sum(10 ** (dbfs / 20.0) * math.sqrt(2) * np.sin(2 * np.pi * fk * tt + i_)
                       for i_, fk in enumerate(freqs))

        def thumps(x: np.ndarray, times: list[float], seed: int) -> np.ndarray:
            """Supply-droop-like thumps: 10-80 Hz, 300 ms Hann, +20 dB over the floor - shaped like rumble."""
            rng = np.random.default_rng(seed)
            m = int(0.3 * FS)
            for t0 in times:
                add(x, t0, _band_noise(rng, m, 10.0, 80.0, -50.0) * np.hanning(m))
            return x

        def train(x: np.ndarray, t0: float, period_s: float, n_: int, burst_s: float, freqs: list[float],
                  dbfs: float) -> np.ndarray:
            """n_ bursts of the summed tones (dbfs RMS each burst), one every period_s from t0."""
            for i_ in range(n_):
                if int((t0 + i_ * period_s + burst_s) * FS) < len(x):
                    add(x, t0 + i_ * period_s, tones(freqs, burst_s, dbfs - 10 * math.log10(len(freqs))))
            return x

        bq = [save("Q0a", rumble(_synth(sec, 61), [15.3, 40.2], 1)),
              save("Q0b", rumble(_synth(sec, 62), [22.7, 51.1], 2, dur_ms=900.0))]
        qclean = save("qclean", rumble(_synth(sec, 63), [12.4, 33.3], 3))
        rep = run(bq, [qclean], "silence", None, trim, [], None, lim)
        expect("rumble bursts without --band-split -> whole-file metrics fail", rep, "qclean", "FAIL")
        # rumble-region (low band) cases: REVIEW unless rumble within the baseline's explains them
        lowc = [qclean, save("qthump2", thumps(_synth(sec, 90), [24.6, 49.3], 30)),
                save("qthumps", thumps(_synth(sec, 91), [9.2, 17.9, 23.1, 34.6, 41.0, 50.3, 62.8], 31)),
                save("qthumpper", thumps(_synth(sec, 92), [14.0 + 7.68 * i for i in range(4)], 32))]
        pat = _band_noise(np.random.default_rng(87), BLOCK, 40.0, 150.0, -45.0)   # one DMA block's worth, repeated
        lowc.append(save("qdma", add(_synth(sec, 87), 30.0, np.tile(pat, 16) * np.hanning(16 * BLOCK))))
        lowc.append(save("qlfline", add(rumble(_synth(sec, 88), [12.4], 28), 0.0, tones([120.0], sec, -85.0))))
        rep = run(bq, lowc, "silence", None, trim, [], None, lim, split=True)
        expect("--band-split: rumbling baseline continuity", rep, "Q0a", "PASS")
        expect("--band-split: rumble in baseline and capture -> PASS", rep, "qclean", "PASS")
        qc = next(c for c in rep["captures"] if c["label"] == "qclean")
        results.append(("... its whole-window low band does not pass, the rumble within the baseline explains it",
                        qc["low_band_verdict"] == "PASS" and qc["high_band_verdict"] == "PASS"
                        and any(r["metric"] == "(L) bursts vs baseline" for r in qc["rows"]),
                        f"high {qc['high_band_verdict']} low {qc['low_band_verdict']}"))
        expect("--band-split: 2 low-frequency thumps (within the baseline's) PASS", rep, "qthump2", "PASS")
        expect("--band-split: 7 low-frequency thumps (more than the baseline) -> REVIEW", rep, "qthumps", "REVIEW",
               ("(L) bursts vs baseline",))
        expect("--band-split: 4 thumps every 7.68 s -> REVIEW", rep, "qthumpper", "REVIEW", ("(L) burst rhythm",))
        expect("--band-split: 128 ms periodic low-band burst -> REVIEW", rep, "qdma", "REVIEW", ("(L) burst rhythm",))
        expect("--band-split: steady 120 Hz line (no burst) -> REVIEW", rep, "qlfline", "REVIEW",
               ("(L) recheck without the bursts",))
        # every injection the exclusion-based quiet mode let through, plus the re-review's: FAIL or INCONCLUSIVE
        rng_i = np.random.default_rng(81)
        inj = []
        x = rumble(_synth(sec, 81), [12.4], 21)
        for t0 in (19.1, 31.6, 38.2, 52.9, 61.0):
            x[int(t0 * FS):int(t0 * FS) + 5, :] += 0.2
        inj.append(save("qclicks", x))
        x = rumble(_synth(sec, 82), [12.4], 22)
        for t0 in (18.4, 30.2, 47.7, 58.1):
            add(x, t0, rng_i.standard_normal(FS) * 10 ** ((-70.0 + 12.0) / 20.0))
        inj.append(save("qhiss", x))
        x = rumble(_synth(sec, 83), [12.4], 23)
        for t0 in (25.0, 50.0):
            add(x, t0, _band_noise(rng_i, FS, 1000.0, 6000.0, -60.0))
        inj.append(save("qhfhiss", x))
        x = rumble(_synth(sec, 84), [12.4], 24)
        t0 = 0.3
        while int(t0 * FS) + 72 < len(x):      # 4.5 ms (72 samples) +30 dB every 10.24 s
            add(x, t0, rng_i.standard_normal(72) * 10 ** ((-70.0 + 30.0) / 20.0))
            t0 += 10.24
        inj.append(save("qsparse", x))
        c266 = [800.0 / 3.0 * k_ for k_ in range(1, 25) if k_ % 3]
        inj.append(save("q266", add(add(rumble(_synth(sec, 85), [12.4], 25), 30.0, tones(c266, 1.5, -70.0)),
                                    55.0, tones(c266, 1.5, -70.0))))
        inj.append(save("q400", add(rumble(_synth(sec, 86), [12.4], 26), 40.0,
                                    tones([400.0 * k_ for k_ in range(1, 20)], 1.5, -70.0))))
        inj.append(save("q800", add(rumble(_synth(sec, 64), [12.4], 4), 45.2,
                                    tones([800.0 * k_ for k_ in range(1, 10)], 1.5, -75.0))))
        inj.append(save("qhidden", rumble(_synth(sec, 65), [12.4, 30.2], 5, dur_ms=900.0, comb_dbfs=-60.0)))
        inj.append(save("qpage", rumble(_synth(sec, 66, bursts=(1.28, 11.25, 10.0)), [12.4], 6)))
        x = rumble(_synth(sec, 88), [12.4], 28)
        x[int(40.0 * FS):int(42.0 * FS), :] += 0.01                 # a DC step up and back down 2 s later
        inj.append(save("qstep", x))
        x = rumble(_synth(sec, 89), [12.4], 29)
        for t0 in (21.3, 37.9, 55.2):
            x[int(t0 * FS), :] += 0.02                                 # single-sample spikes
        inj.append(save("qspikes", x))
        inj.append(save("qfloor", rumble(_synth(sec, 67, floor_dbfs=-69.0), [20.5], 7)))
        # the re-review's cases: in the second right after a rumble burst (what used to be a guard second)
        inj.append(save("q330", add(rumble(_synth(sec, 68), [12.4, 33.3], 8), 12.75, tones([330.0], 1.0, -70.0))))
        inj.append(save("q330far", add(rumble(_synth(sec, 69), [12.4], 9), 40.0, tones([330.0], 1.0, -70.0))))
        inj.append(save("q300n", add(rumble(_synth(sec, 70), [12.4, 33.3], 10), 12.75,
                                     _band_noise(rng_i, FS, 300.0, 480.0, -60.0))))
        inj.append(save("qtrain", train(rumble(_synth(sec, 76), [12.4], 16), 0.3, 1.28, 80, 0.01125,
                                        [330.0, 450.0], -60.0)))
        inj.append(save("qtrain_g", train(rumble(_synth(sec, 77), [12.4, 33.3], 17), 12.75, 0.01125, 80, 0.00125,
                                          [330.0, 450.0], -60.0)))
        rep = run(bq, inj, "silence", None, trim, [], None, lim, split=True)
        for lb_, why_, rows_ in (
                ("qclicks", "5 clicks (0.2 FS)", ("(c) clicks",)),
                ("qhiss", "4 x 1 s broadband hiss +12 dB", ("(a) noise floor 300",)),
                ("qhfhiss", "2 x 1 s 1-6 kHz hiss +10 dB", ("(a) floor octave",)),
                ("qsparse", "4.5 ms +30 dB bursts every 10.24 s", ("(c) clicks",)),
                ("q266", "k*266.67 Hz comb (no 800 Hz teeth)", ("(b) spur targets",)),
                ("q400", "k*400 Hz comb", ("(b) spur targets",)),
                ("q800", "k*800 Hz comb 1.5 s", ("(b) spur targets",)),
                ("qhidden", "comb lines under rumble", ("(b) spur targets",)),
                ("qpage", "page-scan 11.25 ms +10 dB every 1.28 s", ("(c) envelope 0.78125 Hz",)),
                ("qstep", "0.01 FS DC step up and down", ("(c) clicks",)),
                ("qspikes", "3 single-sample spikes 0.02 FS", ("(c) clicks",)),
                ("qfloor", "+1 dB floor", ("(a) noise floor 300",)),
                ("q330", "330 Hz tone 1 s next to rumble", ("(b) new narrow peaks",)),
                ("q330far", "330 Hz tone 1 s in quiet", ("(b) new narrow peaks",)),
                ("q300n", "300-480 Hz noise 1 s +10 dB next to rumble", ("(a) floor octave 300-600",)),
                ("qtrain", "11.25 ms 330+450 Hz bursts every 1.28 s", ("(c) envelope 0.78125 Hz",)),
                ("qtrain_g", "1.25 ms 330+450 Hz bursts every 11.25 ms next to rumble", ("(b) new narrow peaks",))):
            expect(f"--band-split: {why_} -> FAIL", rep, lb_, "FAIL", rows_)
        json.dumps(rep)
        md_ = markdown(rep)
        results.append(("markdown carries both band verdicts", "low band (mechanical rumble region)" in md_,
                        f"{len(md_)} chars"))
        # baselines that disagree in the high band (rumble knocks in one): INCONCLUSIVE, never PASS
        x = rumble(_synth(sec, 93), [14.1, 29.3], 33)
        add(x, 14.2, rng_i.standard_normal(FS // 2) * 10 ** (-50.0 / 20.0) * np.hanning(FS // 2))   # a knock
        bqn = [bq[0], save("Q0n", x)]
        rep = run(bqn, [("qclean", qclean[1]), ("q330far", d / "q330far.wav")], "silence", None, trim, [], None,
                  lim, split=True)
        for lb_ in ("qclean", "q330far"):
            c_ = next(c for c in rep["captures"] if c["label"] == lb_)
            results.append((f"noisy baseline (knock in one repeat): {lb_} not PASS", c_["verdict"] in
                            ("INCONCLUSIVE", "FAIL"), c_["verdict"]))
        # tone captures: the high band carries a 997 Hz tone; the rumble stays in the low band
        bqt = [save("QT0a", rumble(_synth(sec, 71, tone), [15.3], 11)),
               save("QT0b", rumble(_synth(sec, 72, tone), [41.7], 12))]
        x = rumble(_synth(sec, 75, tone), [25.2, 52.9], 15)
        rep = run(bqt, [save("qtclean", rumble(_synth(sec, 73, tone), [25.2], 13)),
                        save("qtnoise", rumble(_synth(sec, 74, tone, floor_dbfs=-67.0), [25.2], 14)),
                        save("qtstep", np.delete(x, 40 * FS, axis=0))],
                  "auto", None, trim, [], None, lim, split=True)
        expect("--band-split tone: rumble in the low band -> PASS", rep, "qtclean", "PASS")
        expect("--band-split tone: +3 dB noise -> SNR", rep, "qtnoise", "FAIL", ("(d) SNR",))
        expect("--band-split tone: 1-sample loss between rumble bursts -> phase step", rep, "qtstep", "FAIL",
               ("(e) tone phase steps",))
        h_ = split_fir(lim)
        fz = np.fft.rfftfreq(1 << 16, 1.0 / FS)
        gz = 20 * np.log10(np.maximum(np.abs(np.fft.rfft(h_, 1 << 16)), 1e-12))
        results.append(("split FIR: <= -95 dB below 300 Hz, +-0.01 dB from 330 Hz",
                        gz[fz <= 300.0].max() <= -95.0 and np.abs(gz[(fz >= 330.0) & (fz < 7990.0)]).max() <= 0.01,
                        f"{len(h_)} taps, stop {gz[fz <= 300.0].max():.1f} dB, pass ripple "
                        f"{np.abs(gz[(fz >= 330.0) & (fz < 7990.0)]).max():.4f} dB"))
        json.dumps(rep)

    bad = 0
    for name, ok, info in results:
        print(f"[{'PASS' if ok else 'FAIL'}] {name}: {info}")
        bad += not ok
    print(f"rf_audio_check self-test: {len(results)} checks, {bad} failures")
    return 1 if bad else 0


# ----------------------------------------------------------------------------------------------------- main
def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--baseline", action="append", default=[], help="[LABEL=]C0.wav (repeat for repeats)")
    ap.add_argument("--capture", action="append", default=[], help="[LABEL=]capture.wav (repeatable)")
    ap.add_argument("--signal", choices=("auto", "silence", "tone"), default="auto")
    ap.add_argument("--tone-hz", type=float, default=None, help="test tone frequency (default: detected)")
    ap.add_argument("--bench-log", action="append", default=[],
                    help="[LABEL=]log - device counters: dev_cycle.py log output, BTBENCH.LOG or saved +XREC:DBG "
                         "replies; LABEL= makes it that capture's own log (searched first)")
    ap.add_argument("--labels", action="append", default=[], type=Path,
                    help="file of 'LABEL REC_NNNN.wav' lines: the device file behind each capture label")
    ap.add_argument("--band-split", "--quiet-mode", action="store_true", dest="band_split",
                    help="split at --lim-split-hz: high band judged on the whole window (FAIL / INCONCLUSIVE), "
                         "low band (rumble region) at most REVIEW; --quiet-mode is an alias (it no longer "
                         "excludes any second)")
    ap.add_argument("--no-device-counters", action="append", default=[], metavar="LABEL",
                    help="with --bench-log: this capture has no counter line on purpose (audio metrics only)")
    ap.add_argument("--expect-seconds", type=float, default=None, help="length check without --bench-log")
    ap.add_argument("--trim-start-s", type=float, default=0.25)
    ap.add_argument("--trim-end-s", type=float, default=0.1)
    ap.add_argument("--json", type=Path)
    ap.add_argument("--md", type=Path)
    ap.add_argument("--self-test", action="store_true")
    for k, v in Limits().__dict__.items():
        ap.add_argument("--lim-" + k.replace("_", "-"), type=type(v), default=v, dest="lim_" + k)
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test()
    if not a.baseline or not a.capture:
        ap.error("--baseline and --capture are required (or --self-test)")
    lim = Limits(**{k: getattr(a, "lim_" + k) for k in Limits().__dict__})
    try:
        dev_names = parse_labels(a.labels)
        base = [parse_capture_arg(s) for s in a.baseline]
        caps = [parse_capture_arg(s) for s in a.capture]
        for lb, _, dn in base + caps:
            if dn:
                dev_names[lb] = dn
        logs: list[Path] = []
        label_logs: dict[str, list[Path]] = {}
        for s_ in a.bench_log:
            if "=" in s_ and not Path(s_).exists():
                lb, p_ = s_.split("=", 1)
                label_logs.setdefault(lb, []).append(Path(p_))
            else:
                logs.append(Path(s_))
        labels = {lb for lb, _, _ in base + caps}
        for lb in label_logs:
            if lb not in labels:
                raise ValueError(f"--bench-log {lb}=...: no capture or baseline labelled {lb}")
        rep = run([(lb, p) for lb, p, _ in base], [(lb, p) for lb, p, _ in caps], a.signal, a.tone_hz,
                  (a.trim_start_s, a.trim_end_s), logs, a.expect_seconds, lim, set(a.no_device_counters),
                  a.band_split, dev_names, label_logs)
    except (OSError, ValueError, wave.Error) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2
    md = markdown(rep)
    print(md)
    if a.md:
        a.md.write_text(md + "\n")
    if a.json:
        a.json.write_text(json.dumps(rep, indent=1) + "\n")
    return 0 if rep["pass"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
