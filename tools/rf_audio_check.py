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

Usage:
  rf_audio_check.py --baseline C0_1.wav --baseline C0_2.wav --capture C3=REC_0012.wav [--capture C1=...]
                    [--bench-log ring.log] [--no-device-counters LABEL] [--signal auto|silence|tone]
                    [--json out.json] [--md out.md]
  rf_audio_check.py --self-test
Exit status: 0 all captures pass, 1 a capture fails or is inconclusive, 2 usage / input error. numpy only (scipy is not needed).

SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import argparse
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


def welch(x: np.ndarray, fs: float, nper: int, window: str = "bh4") -> tuple[np.ndarray, np.ndarray]:
    """One-sided PSD (density), Blackman-Harris (or Hann), 50 % overlap, mean removed per segment."""
    nper = min(nper, len(x))
    hop = max(1, nper // 2)
    w = bh4(nper) if window == "bh4" else np.hanning(nper)
    nseg = 1 + (len(x) - nper) // hop
    idx = np.arange(nper)[None, :] + hop * np.arange(nseg)[:, None]
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


def spur_targets() -> list[tuple[float, str]]:
    seen: dict[float, str] = {}
    for base, kmax in ((1600.0, 99), (800.0, 99), (400.0, 99), (800.0 / 3.0, 99), (40.0, 5)):
        for k in range(1, kmax + 1):
            f = base * k
            if f >= BAND[1]:
                break
            key = round(f, 1)
            seen.setdefault(key, f"{k}x{base:g}" if k > 1 else f"{base:g}")
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


def envelope(x: np.ndarray) -> np.ndarray:
    """RMS envelope at ENV_FS: x^2 low-passed below 45 Hz (brick wall), then decimated. A plain 10 ms mean would alias
    a tone's 2 f0 power ripple into the envelope band (997 Hz -> a 6 Hz line that moves with the tone frequency)."""
    m = int(FS / ENV_FS)
    n = len(x) // m
    p = x[:n * m] ** 2
    spec = np.fft.rfft(p)
    spec[np.fft.rfftfreq(len(p), 1.0 / FS) > 0.45 * ENV_FS] = 0.0
    lp = np.fft.irfft(spec, len(p))
    return np.sqrt(np.maximum(lp[m // 2::m][:n], 0.0))


def env_metrics(x: np.ndarray) -> dict:
    e = envelope(x)
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


def analyse_channel(x: np.ndarray, signal: str, tone_hz: float | None, lim: Limits) -> dict:
    out: dict = {}
    f0 = None
    if signal in ("auto", "tone"):
        f0 = tone_hz or detect_tone(x)
        if signal == "tone" and f0 is None:
            raise ValueError("--signal tone, but no tone found (give --tone-hz)")
    out["signal"] = "tone" if f0 else "silence"
    out["tone_hz"] = round(f0, 3) if f0 else None
    f, p = welch(x, FS, FS)
    band = (f >= BAND[0]) & (f <= BAND[1])
    keep = band & tone_mask(f, f0)
    if not f0:
        out["floor_db"] = round(float(db(p[band].sum())), 3)
        out["floor_a_db"] = round(float(db((p * a_weight(f))[band].sum())), 3)
    else:
        fr, amp, res = tone_fit(x, f0, FS)
        fr_, pr = welch(res, FS, FS)
        bandr = (fr_ >= BAND[0]) & (fr_ <= BAND[1])
        p_fund = float(np.mean(amp ** 2) / 2.0)
        p_thdn = float(pr[bandr].sum())
        p_noise = float(pr[bandr & tone_mask(fr_, f0)].sum())
        out["tone_dbfs"] = round(float(db(p_fund)), 3)
        out["noise_dbfs"] = round(float(db(p_noise)), 3)
        out["snr_db"] = round(float(db(p_fund) - db(p_noise)), 3)
        out["thdn_db"] = round(float(db(p_thdn) - db(p_fund)), 3)
        out["tone_wander_hz"] = round(float(fr.max() - fr.min()), 4) if len(fr) else 0.0
    out["_psd_db"] = db(p)
    out["_keep"] = keep
    out["_f"] = f
    out["env"] = env_metrics(x)
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


def load_capture(label: str, path: Path, signal: str, tone_hz: float | None, trim: tuple[float, float],
                 lim: Limits) -> Capture:
    rate, x = load_wav(path)
    if rate != FS:
        raise ValueError(f"{path}: {rate} Hz, expected {FS}")
    c = Capture(label, path, rate, len(x))
    a, b = int(trim[0] * FS), len(x) - int(trim[1] * FS)
    if b - a < 4 * FS:
        raise ValueError(f"{path}: too short after trimming ({(b - a) / FS:.1f} s, need >= 4 s)")
    c.raw = x
    e = min(int(lim.cont_edge_s * FS), len(x) // 4)
    for k in range(x.shape[1]):
        ch = analyse_channel(x[a:b, k], signal, tone_hz, lim)
        if ch["tone_hz"]:   # continuity: the whole file but the edges (the spectral metrics keep the middle window)
            ch["phase_steps"] = phase_steps(x[e:len(x) - e, k], ch["tone_hz"], lim.phase_rad, e / FS)
        c.ch.append(ch)
    return c


def row(rows: list, metric: str, value, base, delta, limit: str, ok: bool, ch: int | None = None,
        verdict: str | None = None) -> None:
    rows.append({"metric": metric, "ch": ch, "value": value, "baseline": base, "delta": delta, "limit": limit,
                 "verdict": verdict or ("PASS" if ok else "FAIL")})


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


def compare(cap: Capture, base: list[Capture], lim: Limits, trim: tuple[float, float], dev: dict | None,
            expect_s: float | None, dev_state: str = "none", ref_devs: list[dict] | None = None) -> list[dict]:
    """dev_state: "none" (no --bench-log), "found", "missing" (no line for this file) or "waived".
    ref_devs: the baseline captures' counters (the other baselines when cap is one)."""
    rows: list[dict] = []
    ref_devs = ref_devs or []
    if base and len(base[0].ch) != len(cap.ch):
        raise ValueError(f"{cap.path}: {len(cap.ch)} channel(s), baseline has {len(base[0].ch)}")
    if base:   # (b) repeatability of the baseline itself: the bars below are only as good as it
        if len(base) < 2:
            row(rows, "(b) baseline repeatability", len(base), None, None, "needs >= 2 repeats", False,
                verdict="INCONCLUSIVE")
        for k in range(len(cap.ch)):
            if len(base) < 2:
                break
            keep = base[0].ch[k]["_keep"]
            psd = np.array([b.ch[k]["_psd_db"][keep] for b in base])
            off = np.median(psd - psd.mean(axis=0), axis=1)
            sp = float(off.max() - off.min())
            row(rows, "(b) baseline repeatability", rnd(sp), None, None,
                f"median level offset spread <= {lim.base_bin_spread_db} dB", True, k,
                verdict=None if sp <= lim.base_bin_spread_db else "INCONCLUSIVE")
    for k, c in enumerate(cap.ch):
        bk = [b.ch[k] for b in base]
        if bk and any(b["signal"] != c["signal"] for b in bk):
            raise ValueError(f"{cap.path} ch{k}: signal {c['signal']}, baseline differs (same signal needed)")
        # (a) noise floor
        if c["signal"] == "silence" and bk:
            for key, name in (("floor_db", "(a) noise floor"), ("floor_a_db", "(a) noise floor A-wtd")):
                vals = [b[key] for b in bk]
                m = float(np.mean(vals))
                d = c[key] - m
                sp = max(vals) - min(vals)
                v = None
                if abs(d) > lim.floor_db + sp:
                    v = "FAIL"                       # outside even the repeats' own spread
                elif sp > lim.base_spread_db:
                    v = "INCONCLUSIVE"               # the baseline cannot support a +-floor_db bar
                row(rows, name, rnd(c[key]), rnd(m), rnd(d), f"+-{lim.floor_db} dB (spread {sp:.2f})",
                    abs(d) <= lim.floor_db, k, verdict=v)
        # (b) spurs and new peaks
        if bk:
            f = c["_f"]
            env = ref_level([b["_psd_db"] for b in bk])
            keep = c["_keep"]
            worst = None
            fails = []
            for ft, name in spur_targets():
                sel = (np.abs(f - ft) <= SPUR_WIN_HZ) & keep
                if not sel.any():
                    continue
                d = float(c["_psd_db"][sel].max() - env[sel].max())
                if worst is None or d > worst[1]:
                    worst = (name, d)
                if d > lim.spur_db:
                    fails.append(f"{name} Hz +{d:.1f} dB")
            if worst is not None:
                row(rows, "(b) spur targets", f"worst {worst[0]} Hz", "C0 mean+sd", rnd(worst[1]),
                    f"<= +{lim.spur_db} dB", not fails, k)
                if fails:
                    rows[-1]["detail"] = fails[:20]
            pdb = c["_psd_db"]
            med = local_median(pdb, f)
            hit = keep & (pdb - env > lim.spur_db) & (pdb - med >= lim.peak_prom_db)
            peaks = [{"hz": float(f[i]), "excess_db": rnd(pdb[i] - env[i]), "prom_db": rnd(pdb[i] - med[i])}
                     for i in np.nonzero(hit)[0][:20]]
            row(rows, "(b) new narrow peaks", len(peaks), 0, None,
                f"none > +{lim.spur_db} dB with >= {lim.peak_prom_db} dB prominence", not peaks, k)
            if peaks:
                rows[-1]["detail"] = peaks
        # (c) envelope modulation
        if bk and c["env"] and all(b["env"] for b in bk):
            for ft, m in c["env"]["mod"].items():
                bref = float(ref_level([b["env"]["mod"][ft]["db"] for b in bk]))
                d = m["db"] - bref
                ok = not (d > lim.mod_db and m["prom_db"] >= lim.mod_prom_db)
                row(rows, f"(c) envelope {ft} Hz", rnd(m["db"]), rnd(bref), rnd(d),
                    f"line: <= +{lim.mod_db} dB or prom < {lim.mod_prom_db} dB (prom {m['prom_db']:.1f})", ok, k)
            ef, ep, emed = c["env"]["_f"], c["env"]["_pdb"], c["env"]["_med"]
            if all(len(b["env"]["_pdb"]) == len(ep) for b in bk):
                eref = ref_level([b["env"]["_pdb"] for b in bk])
                sel = (ef >= ENV_BAND[0]) & (ef <= ENV_BAND[1])
                hit = sel & (ep - eref > lim.mod_db) & (ep - emed >= lim.mod_prom_db)
                lines = [{"hz": round(float(ef[i]), 3), "excess_db": rnd(ep[i] - eref[i]),
                          "prom_db": rnd(ep[i] - emed[i])} for i in np.nonzero(hit)[0][:20]]
                row(rows, "(c) envelope lines", len(lines), 0, None,
                    f"none {ENV_BAND[0]:g}-{ENV_BAND[1]:g} Hz > +{lim.mod_db} dB with >= {lim.mod_prom_db} dB prom",
                    not lines, k)
                if lines:
                    rows[-1]["detail"] = lines
            bf = max(float(ref_level([b["env"]["fold_db"] for b in bk])), c["env"]["fold_noise_db"])
            d = c["env"]["fold_db"] - bf
            row(rows, "(c) 1.28 s fold depth", rnd(c["env"]["fold_db"]), rnd(bf), rnd(d), f"<= +{lim.fold_db} dB",
                d <= lim.fold_db, k)
        # (d) tone
        if c["signal"] == "tone" and bk:
            bl = float(np.mean([b["tone_dbfs"] for b in bk]))
            dl = c["tone_dbfs"] - bl
            row(rows, "(d) tone level", rnd(c["tone_dbfs"]), rnd(bl), rnd(dl),
                f"+-{lim.tone_level_db} dB (else SNR moves with level)", True, k,
                verdict=None if abs(dl) <= lim.tone_level_db else "INCONCLUSIVE")
            bs = float(np.mean([b["snr_db"] for b in bk]))
            bt = float(np.mean([b["thdn_db"] for b in bk]))
            bn = float(np.mean([b["noise_dbfs"] for b in bk]))
            row(rows, "(d) SNR", rnd(c["snr_db"]), rnd(bs), rnd(c["snr_db"] - bs), f">= -{lim.tone_db} dB",
                c["snr_db"] - bs >= -lim.tone_db, k)
            row(rows, "(d) THD+N", rnd(c["thdn_db"]), rnd(bt), rnd(c["thdn_db"] - bt), f"<= +{lim.tone_db} dB",
                c["thdn_db"] - bt <= lim.tone_db, k)
            row(rows, "(d) residual noise dBFS", rnd(c["noise_dbfs"]), rnd(bn), rnd(c["noise_dbfs"] - bn),
                f"<= +{lim.noise_db} dB", c["noise_dbfs"] - bn <= lim.noise_db, k)
        # (e) phase
        if c["signal"] == "tone":
            st = c["phase_steps"]
            row(rows, "(e) tone phase steps", len(st), 0, None, f"none > {lim.phase_rad} rad", not st, k)
            if st:
                rows[-1]["detail"] = st
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
            "a counter line for this file (ring wrapped? renamed?) or --no-device-counters LABEL", False)
    elif dev_state == "waived":
        row(rows, "(e) device counters", "waived", None, None, "--no-device-counters: audio metrics only", True)
    if dev is not None:
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
def run(base_paths: list[tuple[str, Path]], caps: list[tuple[str, Path]], signal: str, tone_hz: float | None,
        trim: tuple[float, float], bench_logs: list[Path], expect_s: float | None, lim: Limits,
        waive: set[str] | None = None) -> dict:
    waive = waive or set()
    dev = parse_bench_log(bench_logs) if bench_logs else {}

    def lookup(label: str, p: Path) -> tuple[dict | None, str]:
        if not bench_logs:
            return None, "none"
        d = dev.get(p.name.upper())
        if d is not None:
            return d, "found"
        return None, "waived" if label in waive else "missing"

    base = [load_capture(lb, p, signal, tone_hz, trim, lim) for lb, p in base_paths]
    bdev = [lookup(lb, p) for lb, p in base_paths]
    report = {"baseline": [str(p) for _, p in base_paths], "limits": lim.__dict__, "captures": []}
    for i, b in enumerate(base):   # the baseline itself must be continuous; its counters vs the other repeats
        others = [d for j, (d, _) in enumerate(bdev) if j != i and d is not None]
        rows = compare(b, [], lim, trim, bdev[i][0], expect_s, bdev[i][1], others)
        report["captures"].append(summary(b, rows, is_base=True))
    refs = [d for d, _ in bdev if d is not None]
    for lb, p in caps:
        c = load_capture(lb, p, signal, tone_hz, trim, lim)
        d, st = lookup(lb, p)
        rows = compare(c, base, lim, trim, d, expect_s, st, refs)
        report["captures"].append(summary(c, rows, is_base=False))
    report["pass"] = all(c["pass"] for c in report["captures"])
    return report


def summary(c: Capture, rows: list[dict], is_base: bool) -> dict:
    verdicts = {r["verdict"] for r in rows}
    verdict = "FAIL" if "FAIL" in verdicts else ("INCONCLUSIVE" if "INCONCLUSIVE" in verdicts else "PASS")
    per_ch = []
    for ch in c.ch:
        d = {k: v for k, v in ch.items() if not k.startswith("_")}
        if isinstance(d.get("env"), dict):
            d["env"] = {k: v for k, v in d["env"].items() if not k.startswith("_")}
        per_ch.append(d)
    return {"label": c.label, "file": str(c.path), "baseline": is_base, "samples": c.n, "channels": len(c.ch),
            "seconds": round(c.n / FS, 3), "per_channel": per_ch, "rows": rows, "verdict": verdict,
            "pass": verdict == "PASS"}


def markdown(rep: dict) -> str:
    out = ["# RF audio check", "", f"Baseline: {', '.join(rep['baseline'])}", "",
           f"**Overall: {'PASS' if rep['pass'] else 'NOT PASS'}**", ""]
    for c in rep["captures"]:
        kind = "baseline (continuity only)" if c["baseline"] else "capture"
        sig = ", ".join(f"ch{i} {ch['signal']}" + (f" {ch['tone_hz']} Hz at {ch['tone_dbfs']} dBFS" if ch["tone_hz"] else
                        (f" floor {ch.get('floor_db')} dBFS ({ch.get('floor_a_db')} A)" if "floor_db" in ch else ""))
                        for i, ch in enumerate(c["per_channel"]))
        out += [f"## {c['label']} - {c['verdict']}", "",
                f"{kind}: `{c['file']}`, {c['seconds']} s, {c['channels']} ch, {sig}", "",
                "| metric | ch | value | baseline | delta | limit | verdict |", "|---|---|---|---|---|---|---|"]
        for r in c["rows"]:
            ch = "" if r["ch"] is None else r["ch"]
            out.append(f"| {r['metric']} | {ch} | {r['value']} | {r['baseline']} | "
                       f"{'' if r['delta'] is None else r['delta']} | {r['limit']} | **{r['verdict']}** |")
        for r in c["rows"]:
            if r.get("detail"):
                out.append(f"\n- {r['metric']}: `{json.dumps(r['detail'][:8])}`")
        out.append("")
    return "\n".join(out)


def parse_labeled(s: str) -> tuple[str, Path]:
    if "=" in s and not Path(s).exists():
        lb, p = s.split("=", 1)
        return lb, Path(p)
    return Path(s).stem, Path(s)


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
    ap.add_argument("--bench-log", action="append", default=[], type=Path,
                    help="device counters: dev_cycle.py log output, BTBENCH.LOG or saved +XREC:DBG replies")
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
        rep = run([parse_labeled(s) for s in a.baseline], [parse_labeled(s) for s in a.capture], a.signal, a.tone_hz,
                  (a.trim_start_s, a.trim_end_s), a.bench_log, a.expect_seconds, lim, set(a.no_device_counters))
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
