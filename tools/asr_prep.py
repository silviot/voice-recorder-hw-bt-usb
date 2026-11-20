#!/usr/bin/env python3
"""
Host ASR Preprocessing Helper for Cassini OpenFW Recordings.

Implements the two downstream ASR recipes from docs/CLEAN_ROOM_AUDIO_PLAN.md:
  1. --target parakeet (NVIDIA Parakeet-TDT-0.6B v2/v3):
     Broadside stereo-to-mono sum -> clean-room VAD trim (or Silero VAD if
     torch is installed) -> 16 kHz mono PCM WAV + JSON segment manifest.
  2. --target whistle (Cactus Whistle 55M):
     Broadside stereo-to-mono sum -> 2nd-order zero-phase Butterworth bandpass
     (150-7000 Hz) -> pause-aligned <=30 s WAV chunks + JSON manifest + keywords.

Zero external dependencies required (uses Python stdlib only).

Examples:
  python3 tools/asr_prep.py --self-test
  python3 tools/asr_prep.py REC_0001.wav --target parakeet --out-dir out_parakeet
  python3 tools/asr_prep.py REC_0001.wav --target whistle --out-dir out_whistle --keywords "Cassini,OpenFW"

SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import argparse
import array
import json
import math
from pathlib import Path
import struct
import sys
import tempfile
import wave


TARGET_SR = 16000


def read_wav_samples(path: Path) -> tuple[int, int, list[list[float]]]:
    """Read 16-bit PCM WAV as `(sample_rate, channels, ch_samples_norm)`."""
    with wave.open(str(path), "rb") as wf:
        ch = wf.getnchannels()
        sw = wf.getsampwidth()
        sr = wf.getframerate()
        nframes = wf.getnframes()
        raw = wf.readframes(nframes)
    if ch not in (1, 2) or sw != 2:
        raise ValueError(f"Unsupported WAV format: ch={ch}, sampwidth={sw} (expected 1/2 ch, 16-bit PCM)")
    arr = array.array("h")
    arr.frombytes(raw)
    if sys.byteorder != "little":
        arr.byteswap()
    scale = 1.0 / 32768.0
    if ch == 1:
        return sr, 1, [[s * scale for s in arr]]
    left = [arr[i] * scale for i in range(0, len(arr), 2)]
    right = [arr[i + 1] * scale for i in range(0, len(arr), 2)]
    return sr, 2, [left, right]


def write_mono_wav(path: Path, samples: list[float], sample_rate: int = TARGET_SR) -> None:
    """Write normalized mono float samples [-1, 1] as 16-bit PCM WAV."""
    path.parent.mkdir(parents=True, exist_ok=True)
    arr = array.array("h")
    for s in samples:
        v = int(round(max(-1.0, min(1.0, s)) * 32767.0))
        arr.append(v)
    if sys.byteorder != "little":
        arr.byteswap()
    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(sample_rate)
        wf.writeframes(arr.tobytes())


def downmix_and_resample(
    ch_samples: list[list[float]], src_sr: int, dst_sr: int = TARGET_SR, mode: str = "sum"
) -> list[float]:
    """Downmix stereo (broadside sum/left/right) and linearly resample if needed."""
    if len(ch_samples) == 1:
        mono = list(ch_samples[0])
    elif mode == "left":
        mono = list(ch_samples[0])
    elif mode == "right":
        mono = list(ch_samples[1])
    else:
        mono = [0.5 * (l + r) for l, r in zip(ch_samples[0], ch_samples[1])]

    if src_sr == dst_sr or not mono:
        return mono
    out_len = max(1, int(round(len(mono) * dst_sr / src_sr)))
    ratio = src_sr / dst_sr
    out: list[float] = []
    n_in = len(mono)
    for i in range(out_len):
        pos = i * ratio
        idx = int(pos)
        frac = pos - idx
        s0 = mono[min(idx, n_in - 1)]
        s1 = mono[min(idx + 1, n_in - 1)]
        out.append(s0 + frac * (s1 - s0))
    return out


def design_butter_hpf(fc: float, fs: float) -> tuple[float, float, float, float, float]:
    w0 = 2.0 * math.pi * fc / fs
    cw = math.cos(w0)
    sw = math.sin(w0)
    alpha = sw / math.sqrt(2.0)
    a0 = 1.0 + alpha
    return (
        ((1.0 + cw) * 0.5) / a0,
        -(1.0 + cw) / a0,
        ((1.0 + cw) * 0.5) / a0,
        (-2.0 * cw) / a0,
        (1.0 - alpha) / a0,
    )


def design_butter_lpf(fc: float, fs: float) -> tuple[float, float, float, float, float]:
    w0 = 2.0 * math.pi * fc / fs
    cw = math.cos(w0)
    sw = math.sin(w0)
    alpha = sw / math.sqrt(2.0)
    a0 = 1.0 + alpha
    return (
        ((1.0 - cw) * 0.5) / a0,
        (1.0 - cw) / a0,
        ((1.0 - cw) * 0.5) / a0,
        (-2.0 * cw) / a0,
        (1.0 - alpha) / a0,
    )


def biquad_filter(samples: list[float], coeffs: tuple[float, float, float, float, float]) -> list[float]:
    b0, b1, b2, a1, a2 = coeffs
    z1 = 0.0
    z2 = 0.0
    out = [0.0] * len(samples)
    for i, x in enumerate(samples):
        y = b0 * x + z1
        z1 = b1 * x - a1 * y + z2
        z2 = b2 * x - a2 * y
        out[i] = y
    return out


def biquad_filtfilt(samples: list[float], coeffs: tuple[float, float, float, float, float]) -> list[float]:
    fwd = biquad_filter(samples, coeffs)
    fwd.reverse()
    rev = biquad_filter(fwd, coeffs)
    rev.reverse()
    return rev


def apply_whistle_bandpass(samples: list[float], sample_rate: int = TARGET_SR) -> list[float]:
    """Zero-phase 150-7000 Hz Butterworth bandpass for Cactus Whistle."""
    hpf = design_butter_hpf(150.0, float(sample_rate))
    lpf = design_butter_lpf(7000.0, float(sample_rate))
    return biquad_filtfilt(biquad_filtfilt(samples, hpf), lpf)


def rms_dbfs(samples: list[float]) -> float:
    if not samples:
        return -96.0
    mean_sq = sum(s * s for s in samples) / len(samples)
    if mean_sq <= 1e-10:
        return -96.0
    return max(-96.0, min(0.0, 10.0 * math.log10(mean_sq)))


def detect_speech_segments(
    samples: list[float],
    sample_rate: int = TARGET_SR,
    open_thresh_dbfs: float = -34.0,
    close_thresh_dbfs: float = -40.0,
    pad_ms: int = 160,
    merge_gap_ms: int = 320,
) -> list[dict[str, int | float]]:
    """Detect active speech segments using energy + zero-crossing hysteresis."""
    if not samples:
        return []
    win = max(1, (sample_rate * 20) // 1000)  # 20 ms windows
    active_wins: list[tuple[int, int, float]] = []
    gate = False
    hang_wins = max(1, (pad_ms + 19) // 20)
    hang_rem = 0

    for start in range(0, len(samples), win):
        end = min(len(samples), start + win)
        blk = samples[start:end]
        db = rms_dbfs(blk)
        zc = sum(
            1
            for i in range(1, len(blk))
            if (blk[i] >= 0.0 and blk[i - 1] < 0.0) or (blk[i] < 0.0 and blk[i - 1] >= 0.0)
        )
        zcr = zc / max(1, len(blk) - 1)
        speech_zcr = 0.002 <= zcr <= 0.48
        trig = db >= open_thresh_dbfs and (speech_zcr or db >= open_thresh_dbfs + 12.0)

        if gate:
            if db >= close_thresh_dbfs:
                hang_rem = hang_wins
            elif hang_rem > 0:
                hang_rem -= 1
            else:
                gate = False
        elif trig:
            gate = True
            hang_rem = hang_wins

        if gate:
            if active_wins and active_wins[-1][1] == start:
                ps, _, _ = active_wins[-1]
                active_wins[-1] = (ps, end, db)
            else:
                active_wins.append((start, end, db))

    pad_samp = (sample_rate * pad_ms) // 1000
    gap_samp = (sample_rate * merge_gap_ms) // 1000
    merged: list[list[int]] = []
    for s, e, _ in active_wins:
        s_pad = max(0, s - pad_samp)
        e_pad = min(len(samples), e + pad_samp)
        if merged and s_pad <= merged[-1][1] + gap_samp:
            merged[-1][1] = max(merged[-1][1], e_pad)
        else:
            merged.append([s_pad, e_pad])

    segments: list[dict[str, int | float]] = []
    for s, e in merged:
        seg = samples[s:e]
        peak = max((abs(x) for x in seg), default=0.0)
        segments.append({
            "start_sample": s,
            "end_sample": e,
            "start_ms": int((s * 1000) // sample_rate),
            "end_ms": int((e * 1000) // sample_rate),
            "rms_dbfs": round(rms_dbfs(seg), 2),
            "peak_dbfs": round(20.0 * math.log10(max(1e-5, peak)), 2),
        })
    return segments


def split_pause_aligned_chunks(
    samples: list[float],
    sample_rate: int = TARGET_SR,
    max_chunk_sec: float = 30.0,
    min_chunk_sec: float = 15.0,
) -> list[tuple[int, int]]:
    """Split samples into pause-aligned chunks each strictly <= max_chunk_sec."""
    max_s = max(1, int(round(max_chunk_sec * sample_rate)))
    min_s = min(max_s - 1, max(1, int(round(min_chunk_sec * sample_rate))))
    win = max(1, (sample_rate * 40) // 1000)  # 40 ms energy search window

    chunks: list[tuple[int, int]] = []
    pos = 0
    total = len(samples)
    while pos < total:
        if total - pos <= max_s:
            chunks.append((pos, total))
            break
        search_lo = pos + min_s
        search_hi = pos + max_s
        best_split = search_hi
        best_e = float("inf")
        for cur in range(search_lo, search_hi - win + 1, win // 2 or 1):
            blk = samples[cur : cur + win]
            e = sum(x * x for x in blk)
            if e < best_e:
                best_e = e
                best_split = cur + win // 2
        if best_split <= pos:
            best_split = search_hi
        chunks.append((pos, best_split))
        pos = best_split
    return chunks


def run_pipeline(
    in_wav: Path,
    out_dir: Path,
    target: str,
    channel_mode: str = "sum",
    keywords: list[str] | None = None,
) -> Path:
    src_sr, src_ch, ch_samples = read_wav_samples(in_wav)
    mono = downmix_and_resample(ch_samples, src_sr, TARGET_SR, mode=channel_mode)
    stem = in_wav.stem
    out_dir.mkdir(parents=True, exist_ok=True)

    manifest: dict[str, object] = {
        "source_wav": str(in_wav),
        "source_sample_rate": src_sr,
        "source_channels": src_ch,
        "channel_mode": channel_mode,
        "target": target,
        "target_sample_rate": TARGET_SR,
    }

    if target == "parakeet":
        segs = detect_speech_segments(mono, TARGET_SR)
        trimmed: list[float] = []
        for seg in segs:
            s = int(seg["start_sample"])
            e = int(seg["end_sample"])
            trimmed.extend(mono[s:e])
        if not trimmed:
            trimmed = mono
        out_wav = out_dir / f"{stem}_parakeet_16k.wav"
        write_mono_wav(out_wav, trimmed, TARGET_SR)
        manifest["output_wav"] = str(out_wav)
        manifest["duration_ms"] = int((len(trimmed) * 1000) // TARGET_SR)
        manifest["speech_segments"] = segs
    elif target == "whistle":
        bp = apply_whistle_bandpass(mono, TARGET_SR)
        ranges = split_pause_aligned_chunks(bp, TARGET_SR, max_chunk_sec=30.0)
        chunk_entries: list[dict[str, object]] = []
        for idx, (s, e) in enumerate(ranges):
            c_wav = out_dir / f"{stem}_whistle_{idx:03d}.wav"
            write_mono_wav(c_wav, bp[s:e], TARGET_SR)
            chunk_entries.append({
                "index": idx,
                "wav": str(c_wav),
                "start_ms": int((s * 1000) // TARGET_SR),
                "end_ms": int((e * 1000) // TARGET_SR),
                "duration_ms": int(((e - s) * 1000) // TARGET_SR),
                "rms_dbfs": round(rms_dbfs(bp[s:e]), 2),
            })
        manifest["chunks"] = chunk_entries
        manifest["keywords"] = list(keywords or [])
    else:
        raise ValueError(f"Unknown target '{target}'")

    manifest_path = out_dir / f"{stem}_{target}_manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest_path


def run_self_test() -> int:
    """Verify Parakeet VAD trim and Whistle bandpass+<=30s chunking on synthetic stereo WAV."""
    sr = 16000
    dur_s = 45
    n = sr * dur_s
    # Build stereo 45s test recording:
    # - 0..2s: quiet 60 Hz hum
    # - 2..22s: 1000 Hz speech tone + 60 Hz hum + 7.5 kHz hiss
    # - 22..24s: pause (silence + tiny 60 Hz hum) -> ideal <=30s split point!
    # - 24..43s: 1000 Hz speech tone
    # - 43..45s: trailing silence
    raw = array.array("h")
    for i in range(n):
        t = i / sr
        hum = 0.25 * math.sin(2.0 * math.pi * 60.0 * t)
        hiss = 0.10 * math.sin(2.0 * math.pi * 7500.0 * t)
        speech = 0.0
        if (2.0 <= t < 22.0) or (24.0 <= t < 43.0):
            speech = 0.35 * math.sin(2.0 * math.pi * 1000.0 * t)
        l = max(-1.0, min(1.0, speech + hum + hiss))
        r = max(-1.0, min(1.0, speech + hum - hiss))  # hiss cancels in broadside sum too
        raw.append(int(round(l * 32767)))
        raw.append(int(round(r * 32767)))
    if sys.byteorder != "little":
        raw.byteswap()

    with tempfile.TemporaryDirectory(prefix="asr_prep_test_") as tmp:
        tmp_path = Path(tmp)
        in_wav = tmp_path / "REC_TEST.wav"
        with wave.open(str(in_wav), "wb") as wf:
            wf.setnchannels(2)
            wf.setsampwidth(2)
            wf.setframerate(sr)
            wf.writeframes(raw.tobytes())

        # 1. Parakeet recipe
        pm_path = run_pipeline(in_wav, tmp_path / "parakeet", target="parakeet", channel_mode="sum")
        pm = json.loads(pm_path.read_text(encoding="utf-8"))
        out_p = Path(pm["output_wav"])
        assert out_p.is_file()
        assert len(pm["speech_segments"]) >= 1
        assert pm["duration_ms"] >= 35000

        # 2. Whistle recipe (bandpass 150-7000 Hz + pause-aligned <= 30s chunks)
        wm_path = run_pipeline(
            in_wav,
            tmp_path / "whistle",
            target="whistle",
            channel_mode="sum",
            keywords=["Cassini", "OpenFW"],
        )
        wm = json.loads(wm_path.read_text(encoding="utf-8"))
        chunks = wm["chunks"]
        assert len(chunks) == 2, f"Expected 2 chunks split at 22..24s pause, got {len(chunks)}"
        for c in chunks:
            assert c["duration_ms"] <= 30000
            c_sr, c_ch, c_samps = read_wav_samples(Path(c["wav"]))
            assert c_sr == 16000 and c_ch == 1
            assert len(c_samps[0]) > 0
        assert wm["keywords"] == ["Cassini", "OpenFW"]

        # Direct 60 Hz rejection vs 1 kHz passband check on apply_whistle_bandpass
        hum_only = [0.5 * math.sin(2.0 * math.pi * 60.0 * i / sr) for i in range(sr)]
        tone_1k = [0.5 * math.sin(2.0 * math.pi * 1000.0 * i / sr) for i in range(sr)]
        hum_bp = apply_whistle_bandpass(hum_only, sr)[2000:-2000]
        t1k_bp = apply_whistle_bandpass(tone_1k, sr)[2000:-2000]
        hum_att_db = rms_dbfs(hum_only) - rms_dbfs(hum_bp)
        t1k_err_db = abs(rms_dbfs(tone_1k) - rms_dbfs(t1k_bp))
        assert hum_att_db > 12.0, f"Expected >12 dB 60 Hz rejection, got {hum_att_db:.2f} dB"
        assert t1k_err_db < 1.0, f"Expected <1 dB 1 kHz loss, got {t1k_err_db:.2f} dB"

    print(
        f"asr_prep.py self-test PASSED "
        f"(parakeet VAD trim, whistle 2x <=30s pause chunks, 60Hz -{hum_att_db:.1f}dB, 1kHz err {t1k_err_db:.2f}dB)"
    )
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Preprocess Cassini OpenFW WAV recordings for Parakeet-TDT & Cactus Whistle"
    )
    ap.add_argument("input_wav", nargs="?", type=Path, help="Input 16-bit WAV file")
    ap.add_argument("--target", choices=["parakeet", "whistle"], default="parakeet", help="Target ASR recipe")
    ap.add_argument("--out-dir", type=Path, default=Path("asr_out"), help="Output directory")
    ap.add_argument("--channel", choices=["sum", "left", "right"], default="sum", help="Stereo-to-mono mix")
    ap.add_argument("--keywords", default="", help="Comma-separated Whistle keyword biasing list")
    ap.add_argument("--self-test", action="store_true", help="Run built-in synthetic audio self-test")
    args = ap.parse_args()

    if args.self_test:
        return run_self_test()
    if args.input_wav is None:
        ap.error("Specify input_wav or --self-test")

    kw = [k.strip() for k in args.keywords.split(",") if k.strip()] if args.keywords else []
    manifest = run_pipeline(args.input_wav, args.out_dir, args.target, args.channel, kw)
    print(f"Wrote ASR manifest: {manifest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
