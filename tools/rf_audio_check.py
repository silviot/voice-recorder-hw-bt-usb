#!/usr/bin/env python3
"""
rf_audio_check.py - "does the radio change the recording?" (docs/BT_SYNC_SPEED_AND_RECORDING.md section 3, M2).

Compares WAV captures from the recorder (16 kHz, 16-bit PCM, mono or stereo) against a baseline (C0: radio never on,
same gain / DSP profile / channels / signal) and prints a pass/fail table per capture, as Markdown and JSON.

Metrics (thresholds are the design's M2 bar, each configurable):
  (a) noise floor 20 Hz - 8 kHz, unweighted and A-weighted (dBFS, RMS of full scale = 0 dB; silence captures)
      PASS: within baseline +- 0.5 dB (baseline = mean of the baseline repeats; their spread is reported)
  (b) Welch PSD, 1 Hz bins (Hann, 1 s segments, 50 % overlap):
      spurs   - the strongest bin within +-2 Hz of every target k*1600, k*800, k*400, k*266.7 (< 8 kHz) and k*40 Hz
                (k <= 5): PASS if no target exceeds the baseline's same window (max over the repeats) by > 3 dB
      peaks   - any 1 Hz bin 20 Hz - 7.99 kHz that exceeds the baseline envelope by > 3 dB AND stands >= 6 dB above
                its own +-50 Hz median (a narrow line, not noise statistics): PASS if there is none
      (bins within +-5 Hz of the test tone's harmonics are excluded in tone captures)
  (c) envelope modulation: 10 ms RMS envelope -> Welch (12.8 s segments) at 0.78, 2.34 and 7.5 Hz (page scan
      1.28 s and its harmonics, 7.5 Hz): FAIL if a target exceeds the baseline by > 3 dB AND stands >= 6 dB above the
      envelope spectrum's median (a line); fold: the envelope averaged over 1.28 s periods, depth = std / mean,
      FAIL if > max(baseline, the same envelope folded at non-1.28 s periods) + 3 dB
  (d) tone captures: SNR and THD+N of the test tone (auto-detected, or --tone-hz), each in 1 s blocks;
      PASS if not worse than the baseline by more than 0.5 dB
  (e) continuity (absolute, no baseline; any finding fails the capture, whatever the other metrics show):
      zero runs >= 2048 samples (the recorder's overrun fill), tone phase steps > 0.05 rad (16 ms blocks;
      a sample-exact loss of 2048 samples is invisible at exactly 1000 Hz: use 997 Hz), length = 16000 * T
      +- 2048 (T from the device's "BENCH rec" counters, --bench-log, or --expect-seconds), and with --bench-log the
      device counters: ov = 0, lost = 0, acct = 0 and the file's samples = wav_smp.

Usage:
  rf_audio_check.py --baseline C0_1.wav --baseline C0_2.wav --capture C3=REC_0012.wav [--capture C1=...]
                    [--bench-log ring.log] [--signal auto|silence|tone] [--json out.json] [--md out.md]
  rf_audio_check.py --self-test
Exit status: 0 all captures pass, 1 a capture fails, 2 usage / input error. numpy only (scipy is not needed).

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
TONE_EXCL_HZ = 5.0
ENV_FS = 100.0                   # 10 ms envelope
ENV_SEG = 1280                   # 12.8 s envelope segments (0.078 Hz bins; 0.78125 Hz = 1 / 1.28 s is bin 10)
FOLD_PERIOD = 128                # 1.28 s in envelope samples
MOD_TARGETS = (0.78125, 2.34375, 7.5)


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
    """Device counters per WAV file name (upper case) from "BENCH rec ...", "BENCH final rec ..." or
    "+XREC:DBG,REC,i,..." / "+XREC:DBG,LIVE,..." lines; the last line for a file wins."""
    out: dict[str, dict[str, str]] = {}
    for p in paths:
        for line in p.read_text(errors="replace").splitlines():
            if "file=" not in line or ("BENCH" not in line and "+XREC:DBG" not in line):
                continue
            kv = dict(t.split("=", 1) for t in re.split(r"[ ,]+", line.strip()) if "=" in t)
            if kv.get("file", "-") not in ("", "-"):
                out[kv["file"].upper()] = kv
    return out


# ----------------------------------------------------------------------------------------------------- spectra
def welch(x: np.ndarray, fs: float, nper: int) -> tuple[np.ndarray, np.ndarray]:
    """One-sided PSD (density), Hann, 50 % overlap, mean removed per segment."""
    nper = min(nper, len(x))
    hop = max(1, nper // 2)
    w = np.hanning(nper)
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
    """Median of the PSD (dB) within +-half_hz of every bin, excluding +-excl_hz around it (1 Hz bins)."""
    n = len(p_db)
    out = np.empty(n)
    h, e = int(half_hz), int(excl_hz)
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


def phase_steps(x: np.ndarray, f0: float, limit: float) -> list[dict]:
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
    hits = np.nonzero((np.abs(step) > limit) & ok)[0]
    return [{"t_s": round((i + 1) * blk / FS, 3), "rad": round(float(step[i]), 3)} for i in hits[:50]]


def zero_runs(x2d: np.ndarray, min_len: int) -> list[dict]:
    z = np.all(x2d == 0.0, axis=1).astype(np.int8)
    edges = np.diff(np.r_[0, z, 0])
    starts, ends = np.nonzero(edges == 1)[0], np.nonzero(edges == -1)[0]
    return [{"t_s": round(s / FS, 3), "samples": int(e - s)} for s, e in zip(starts, ends) if e - s >= min_len]


def envelope(x: np.ndarray) -> np.ndarray:
    m = int(FS / ENV_FS)
    n = len(x) // m
    return np.sqrt((x[:n * m].reshape(n, m) ** 2).mean(axis=1))


def env_metrics(x: np.ndarray) -> dict:
    e = envelope(x)
    mean = float(e.mean())
    if len(e) < 2 * FOLD_PERIOD or mean <= 0:
        return {}
    ed = e - np.polyval(np.polyfit(np.arange(len(e)), e, 1), np.arange(len(e)))
    f, p = welch(ed, ENV_FS, min(ENV_SEG, len(ed)))
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
    return {"mod": mod, "fold_db": round(fold, 2), "fold_noise_db": round(noise, 2)}


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
        _, amp, res = block_fit(x, f0, FS)
        fr, pr = welch(res, FS, FS)
        bandr = (fr >= BAND[0]) & (fr <= BAND[1])
        p_fund = float(np.mean(amp ** 2) / 2.0)
        p_thdn = float(pr[bandr].sum())
        p_noise = float(pr[bandr & tone_mask(fr, f0)].sum())
        out["tone_dbfs"] = round(float(db(p_fund)), 3)
        out["snr_db"] = round(float(db(p_fund) - db(p_noise)), 3)
        out["thdn_db"] = round(float(db(p_thdn) - db(p_fund)), 3)
        out["phase_steps"] = phase_steps(x, f0, lim.phase_rad)
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
    for k in range(x.shape[1]):
        c.ch.append(analyse_channel(x[a:b, k], signal, tone_hz, lim))
    return c


def row(rows: list, metric: str, value, base, delta, limit: str, ok: bool, ch: int | None = None) -> None:
    rows.append({"metric": metric, "ch": ch, "value": value, "baseline": base, "delta": delta, "limit": limit,
                 "verdict": "PASS" if ok else "FAIL"})


def rnd(v, n=2):
    return None if v is None else round(float(v), n)


def compare(cap: Capture, base: list[Capture], lim: Limits, trim: tuple[float, float], dev: dict | None,
            expect_s: float | None) -> list[dict]:
    rows: list[dict] = []
    if base and len(base[0].ch) != len(cap.ch):
        raise ValueError(f"{cap.path}: {len(cap.ch)} channel(s), baseline has {len(base[0].ch)}")
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
                row(rows, name, rnd(c[key]), rnd(m), rnd(d), f"+-{lim.floor_db} dB (spread {max(vals) - min(vals):.2f})",
                    abs(d) <= lim.floor_db, k)
        # (b) spurs and new peaks
        if bk:
            f = c["_f"]
            env = np.max([b["_psd_db"] for b in bk], axis=0)
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
                row(rows, "(b) spur targets", f"worst {worst[0]} Hz", "C0 max", rnd(worst[1]),
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
                bmax = max(b["env"]["mod"][ft]["db"] for b in bk)
                d = m["db"] - bmax
                ok = not (d > lim.mod_db and m["prom_db"] >= lim.mod_prom_db)
                row(rows, f"(c) envelope {ft} Hz", rnd(m["db"]), rnd(bmax), rnd(d),
                    f"line: <= +{lim.mod_db} dB or prom < {lim.mod_prom_db} dB (prom {m['prom_db']:.1f})", ok, k)
            bf = max(max(b["env"]["fold_db"] for b in bk), c["env"]["fold_noise_db"])
            d = c["env"]["fold_db"] - bf
            row(rows, "(c) 1.28 s fold depth", rnd(c["env"]["fold_db"]), rnd(bf), rnd(d), f"<= +{lim.fold_db} dB",
                d <= lim.fold_db, k)
        # (d) tone
        if c["signal"] == "tone" and bk:
            bs = float(np.mean([b["snr_db"] for b in bk]))
            bt = float(np.mean([b["thdn_db"] for b in bk]))
            row(rows, "(d) SNR", rnd(c["snr_db"]), rnd(bs), rnd(c["snr_db"] - bs), f">= -{lim.tone_db} dB",
                c["snr_db"] - bs >= -lim.tone_db, k)
            row(rows, "(d) THD+N", rnd(c["thdn_db"]), rnd(bt), rnd(c["thdn_db"] - bt), f"<= +{lim.tone_db} dB",
                c["thdn_db"] - bt <= lim.tone_db, k)
        # (e) phase
        if c["signal"] == "tone":
            st = c["phase_steps"]
            row(rows, "(e) tone phase steps", len(st), 0, None, f"none > {lim.phase_rad} rad", not st, k)
            if st:
                rows[-1]["detail"] = st
    # (e) zero runs (whole file minus the start trim) and length
    a = int(trim[0] * FS)
    zr = zero_runs(cap.raw[a:], lim.zero_run)
    for z in zr:
        z["t_s"] = round(z["t_s"] + trim[0], 3)
    row(rows, "(e) zero runs", len(zr), 0, None, f"none >= {lim.zero_run} samples", not zr)
    if zr:
        rows[-1]["detail"] = zr[:20]
    n = cap.n
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
    elif expect_s is not None:
        e = int(round(expect_s * FS))
        row(rows, "(e) length vs --expect-seconds", n, e, n - e, f"+-{lim.len_tol}", abs(n - e) <= lim.len_tol)
    else:
        row(rows, "(e) length", n, None, None, "not checked (no --bench-log / --expect-seconds)", True)
    return rows


# ----------------------------------------------------------------------------------------------------- report
def run(base_paths: list[tuple[str, Path]], caps: list[tuple[str, Path]], signal: str, tone_hz: float | None,
        trim: tuple[float, float], bench_logs: list[Path], expect_s: float | None, lim: Limits) -> dict:
    dev = parse_bench_log(bench_logs) if bench_logs else {}
    base = [load_capture(lb, p, signal, tone_hz, trim, lim) for lb, p in base_paths]
    report = {"baseline": [str(p) for _, p in base_paths], "limits": lim.__dict__, "captures": []}
    for b in base:   # the baseline itself must be continuous
        rows = compare(b, [], lim, trim, dev.get(b.path.name.upper()) if bench_logs else None, expect_s)
        report["captures"].append(summary(b, rows, is_base=True))
    for lb, p in caps:
        c = load_capture(lb, p, signal, tone_hz, trim, lim)
        rows = compare(c, base, lim, trim, dev.get(p.name.upper()) if bench_logs else None, expect_s)
        report["captures"].append(summary(c, rows, is_base=False))
    report["pass"] = all(c["pass"] for c in report["captures"])
    return report


def summary(c: Capture, rows: list[dict], is_base: bool) -> dict:
    return {"label": c.label, "file": str(c.path), "baseline": is_base, "samples": c.n, "channels": len(c.ch),
            "seconds": round(c.n / FS, 3),
            "per_channel": [{k: v for k, v in ch.items() if not k.startswith("_")} for ch in c.ch],
            "rows": rows, "pass": all(r["verdict"] == "PASS" for r in rows)}


def markdown(rep: dict) -> str:
    out = ["# RF audio check", "", f"Baseline: {', '.join(rep['baseline'])}", "",
           f"**Overall: {'PASS' if rep['pass'] else 'FAIL'}**", ""]
    for c in rep["captures"]:
        kind = "baseline (continuity only)" if c["baseline"] else "capture"
        sig = ", ".join(f"ch{i} {ch['signal']}" + (f" {ch['tone_hz']} Hz at {ch['tone_dbfs']} dBFS" if ch["tone_hz"] else
                        (f" floor {ch.get('floor_db')} dBFS ({ch.get('floor_a_db')} A)" if "floor_db" in ch else ""))
                        for i, ch in enumerate(c["per_channel"]))
        out += [f"## {c['label']} - {'PASS' if c['pass'] else 'FAIL'}", "",
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
def _synth(seconds: float, seed: int, tone_hz: float | None = None, ch: int = 1, floor_dbfs: float = -70.0,
           spur: tuple[float, float] | None = None, am: tuple[float, float] | None = None) -> np.ndarray:
    rng = np.random.default_rng(seed)
    n = int(seconds * FS)
    t = np.arange(n) / FS
    out = np.empty((n, ch))
    for k in range(ch):
        x = rng.standard_normal(n) * 10 ** (floor_dbfs / 20.0)
        x += 10 ** (-80 / 20.0) * math.sqrt(2) * np.sin(2 * np.pi * 50.0 * t + k)        # mains hum, every capture
        if tone_hz:
            g = 1.0 + (am[1] * np.sin(2 * np.pi * am[0] * t) if am else 0.0)
            x += 0.1 * g * np.sin(2 * np.pi * tone_hz * t + 0.3 * k)                      # -20 dBFS peak
        if spur:
            x += 10 ** (spur[1] / 20.0) * math.sqrt(2) * np.sin(2 * np.pi * spur[0] * t)
        out[:, k] = x
    return out


def self_test() -> int:
    lim = Limits()
    trim = (0.25, 0.1)
    results: list[tuple[str, bool, str]] = []

    def expect(name: str, rep: dict, label: str, want_pass: bool, want_fail_metrics: tuple[str, ...] = ()) -> None:
        c = next(x for x in rep["captures"] if x["label"] == label)
        failed = sorted({r["metric"] for r in c["rows"] if r["verdict"] == "FAIL"})
        ok = c["pass"] == want_pass and all(any(m in f for f in failed) for m in want_fail_metrics)
        if want_pass is False and not want_fail_metrics:
            ok = ok and bool(failed)
        results.append((name, ok, f"pass={c['pass']} failed={failed}"))

    with tempfile.TemporaryDirectory(prefix="rf_audio_check_") as td:
        d = Path(td)
        sec = 30.0
        # silence: two baseline repeats, a clean capture, a 1600 Hz spur 9 dB above the 1 Hz bin noise (the noise
        # floor does not move), a 1 dB raised floor, a 2048-sample dropout, stereo
        for name, x in {
            "c0a": _synth(sec, 1), "c0b": _synth(sec, 2), "clean": _synth(sec, 3),
            "spur": _synth(sec, 4, spur=(1600.0, -100.0)), "floor": _synth(sec, 5, floor_dbfs=-69.0),
            "s0a": _synth(sec, 6, ch=2), "s0b": _synth(sec, 7, ch=2), "sspur": _synth(sec, 8, ch=2, spur=(266.67, -98.0)),
        }.items():
            save_wav(d / f"{name}.wav", x)
        drop = _synth(sec, 9)
        drop[10 * FS:10 * FS + BLOCK] = 0.0
        save_wav(d / "drop.wav", drop)
        b = [("C0a", d / "c0a.wav"), ("C0b", d / "c0b.wav")]
        caps = [("clean", d / "clean.wav"), ("spur", d / "spur.wav"), ("floor", d / "floor.wav"),
                ("drop", d / "drop.wav")]
        rep = run(b, caps, "auto", None, trim, [], None, lim)
        expect("silence baseline continuity", rep, "C0a", True)
        expect("silence clean vs C0", rep, "clean", True)
        expect("1600 Hz spur detected", rep, "spur", False, ("(b) spur targets",))
        spur_c = next(x for x in rep["captures"] if x["label"] == "spur")
        fl = next(r for r in spur_c["rows"] if r["metric"] == "(a) noise floor")
        results.append(("spur leaves the floor within 0.5 dB", fl["verdict"] == "PASS", f"floor delta {fl['delta']}"))
        expect("+1 dB noise floor detected", rep, "floor", False, ("(a) noise floor",))
        expect("2048-sample dropout detected", rep, "drop", False, ("(e) zero runs",))
        rep = run([("S0a", d / "s0a.wav"), ("S0b", d / "s0b.wav")], [("sspur", d / "sspur.wav")], "auto", None,
                  trim, [], None, lim)
        expect("stereo: 266.7 Hz spur in both channels", rep, "sspur", False, ("(b) spur targets",))

        # tone (997 Hz): baselines, clean, one deleted sample (phase step), 2048 deleted samples (silent block loss,
        # phase step + length), a zero block (dropout), 0.78 Hz AM (page-scan rhythm), +3 dB noise (SNR)
        tone = 997.0
        for name, x in {"t0a": _synth(sec, 11, tone), "t0b": _synth(sec, 12, tone), "tclean": _synth(sec, 13, tone),
                        "tam": _synth(sec, 14, tone, am=(0.78125, 0.003)),
                        "tnoise": _synth(sec, 15, tone, floor_dbfs=-67.0)}.items():
            save_wav(d / f"{name}.wav", x)
        x = _synth(sec, 16, tone)
        save_wav(d / "tstep.wav", np.delete(x, 12 * FS, axis=0))
        x = _synth(sec, 17, tone)
        save_wav(d / "tloss.wav", np.delete(x, np.s_[12 * FS:12 * FS + BLOCK], axis=0))
        x = _synth(sec, 18, tone)
        x[12 * FS:12 * FS + BLOCK] = 0.0
        save_wav(d / "tzero.wav", x)
        caps = [(n, d / f"{n}.wav") for n in ("tclean", "tstep", "tloss", "tzero", "tam", "tnoise")]
        rep = run([("T0a", d / "t0a.wav"), ("T0b", d / "t0b.wav")], caps, "auto", None, trim, [], sec, lim)
        expect("tone baseline continuity + length", rep, "T0a", True)
        expect("tone clean vs C0", rep, "tclean", True)
        expect("1-sample loss -> phase step", rep, "tstep", False, ("(e) tone phase steps",))
        expect("2048-sample loss -> phase step (one block is within the length tolerance)", rep, "tloss", False,
               ("(e) tone phase steps",))
        expect("zero block in a tone", rep, "tzero", False, ("(e) zero runs",))
        expect("0.78 Hz AM (0.3 %) detected", rep, "tam", False, ("(c) envelope 0.78125 Hz",))
        expect("+3 dB noise under the tone -> SNR", rep, "tnoise", False, ("(d) SNR",))
        tc = next(x for x in rep["captures"] if x["label"] == "tclean")
        f0 = tc["per_channel"][0]["tone_hz"]
        results.append(("tone frequency estimate", abs(f0 - tone) < 0.05, f"{f0} Hz"))

        # device counters (--bench-log): a matching file passes, a log with an overrun and a length mismatch fails
        x = _synth(sec, 19)
        save_wav(d / "REC_0001.wav", x)
        save_wav(d / "REC_0002.wav", x)
        n = len(x)
        (d / "ring.log").write_text(
            f"BENCH rec seq=1 file=REC_0001.wav rec=0 ch=1 gain=20 dsp=3 vor=0 t_ms=30000 dma_smp={n} "
            f"clk_smp={n + 1000} drift=-1000 wav_smp={n} ov=0 comp=0 lost=0 acct=0\n"
            f"+XREC:DBG,REC,0,seq=2,file=REC_0002.wav,rec=0,ch=1,gain=20,dsp=3,vor=0,t_ms=30000,dma_smp={n},"
            f"clk_smp={n + 5000},drift=-5000,wav_smp={n - 2048},ov=1,comp=0,lost=1,acct=0\r\n")
        rep = run(b, [("dev_ok", d / "REC_0001.wav"), ("dev_bad", d / "REC_0002.wav")], "auto", None, trim,
                  [d / "ring.log"], None, lim)
        expect("device counters clean", rep, "dev_ok", True)
        expect("device counters: overrun, length, wav_smp", rep, "dev_bad", False,
               ("(e) device ov", "(e) length vs device clock", "(e) samples = device wav_smp", "(e) device lost"))
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
                  (a.trim_start_s, a.trim_end_s), a.bench_log, a.expect_seconds, lim)
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
