#!/usr/bin/env python3
"""Convert a Cassini Ogg Opus meeting file into SL6801 open_fw .wav + .cwx files.

Reads a Cassini portable-meeting Ogg Opus container (``org.cassini.portable-meeting/1``),
validates the Vorbis comments (OpusTags), manifest chunks, SHA-256 digests, and
selected ``cassini.words.v1`` transcript without reordering speaker turns, and writes:

  1. ``<stem>.wav`` — 16 kHz mono Microsoft IMA-ADPCM (default, ~8.1 KB/s) or
     16 kHz mono 16-bit PCM (with ``--pcm``), compatible with ``open_fw/src/wav.c``.
  2. ``<stem>.cwx`` — compact CWX1 binary timed-word index with second-granularity
     seek table and speaker palette, compatible with ``open_fw/src/cwx.c``.
"""

from __future__ import annotations

import argparse
import array
import base64
import gzip
import hashlib
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path
from typing import Any, Callable, Dict, Iterable, List, Optional, Sequence, Tuple


CWX_MAGIC = b"CWX1"
CWX_VERSION = 1
CWX_MAX_SPEAKERS = 16
CWX_MAX_TITLE_BYTES = 31
CWX_MAX_LABEL_BYTES = 19
CWX_MAX_WORD_BYTES = 47
CWX_FLAG_TURN_START = 0x01
CWX_FLAG_LOW_CONF = 0x02

SAMPLE_RATE_HZ = 16000
IMA_BLOCK_ALIGN = 256
IMA_SAMPLES_PER_BLOCK = 505  # 1 sample in 4-byte block header + 252 bytes * 2 nibbles

IMA_STEP_TABLE: Tuple[int, ...] = (
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17,
    19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118,
    130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
    337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
    2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358,
    5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
    15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
)

IMA_INDEX_TABLE: Tuple[int, ...] = (
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
)

DEFAULT_SPEAKER_PALETTE_RGB: Tuple[Tuple[int, int, int], ...] = (
    (80, 225, 255),   # cyan
    (255, 195, 70),   # amber
    (115, 240, 145),  # mint
    (255, 130, 170),  # coral
    (185, 160, 255),  # lavender
    (130, 190, 255),  # sky
    (255, 165, 110),  # peach
    (195, 240, 95),   # lime
    (255, 115, 115),  # salmon
    (110, 235, 210),  # teal
    (245, 220, 110),  # gold
    (220, 150, 255),  # orchid
    (155, 220, 255),  # ice
    (255, 185, 185),  # rose
    (175, 245, 175),  # sage
    (210, 210, 210),  # silver (also Unknown/Other fallback)
)

_TYPO_TRANSLATION = str.maketrans({
    "\u2018": "'",
    "\u2019": "'",
    "\u201a": "'",
    "\u201c": '"',
    "\u201d": '"',
    "\u201e": '"',
    "\u2013": "-",
    "\u2014": "-",
    "\u2212": "-",
    "\u2026": "...",
    "\u00a0": " ",
})

_B64URL_RE = re.compile(rb"^[A-Za-z0-9_-]*={0,2}$")


def rgb565(r: int, g: int, b: int) -> int:
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | ((b & 0xF8) >> 3)


def _ogg_crc_table() -> Tuple[int, ...]:
    table: List[int] = []
    for i in range(256):
        r = i << 24
        for _ in range(8):
            if r & 0x80000000:
                r = ((r << 1) ^ 0x04C11DB7) & 0xFFFFFFFF
            else:
                r = (r << 1) & 0xFFFFFFFF
        table.append(r)
    return tuple(table)


_OGG_CRC_TABLE = _ogg_crc_table()


def ogg_crc32(page_bytes: bytes) -> int:
    """Compute canonical Ogg page CRC-32 with CRC field zeroed."""
    buf = bytearray(page_bytes)
    if len(buf) >= 26:
        buf[22:26] = b"\x00\x00\x00\x00"
    crc = 0
    for b in buf:
        crc = ((crc << 8) ^ _OGG_CRC_TABLE[((crc >> 24) & 0xFF) ^ b]) & 0xFFFFFFFF
    return crc


def parse_ogg_packets(
    ogg_bytes: bytes,
    verify_crc: bool = True,
    max_packets: Optional[int] = None,
) -> List[bytes]:
    """Parse Ogg pages and reassemble packets (supporting multi-page packets)."""
    packets: List[bytes] = []
    cur_pkt = bytearray()
    pos = 0
    total = len(ogg_bytes)

    while pos < total:
        if max_packets is not None and len(packets) >= max_packets and not cur_pkt:
            break
        if pos + 27 > total or ogg_bytes[pos : pos + 4] != b"OggS":
            raise ValueError(f"Invalid Ogg page header at offset {pos}")
        version = ogg_bytes[pos + 4]
        if version != 0:
            raise ValueError(f"Unsupported Ogg version {version} at offset {pos}")
        header_type = ogg_bytes[pos + 5]
        page_crc = struct.unpack_from("<I", ogg_bytes, pos + 22)[0]
        num_segs = ogg_bytes[pos + 26]
        seg_table_end = pos + 27 + num_segs
        if seg_table_end > total:
            raise ValueError(f"Truncated Ogg segment table at offset {pos}")
        seg_table = ogg_bytes[pos + 27 : seg_table_end]
        payload_len = sum(seg_table)
        page_end = seg_table_end + payload_len
        if page_end > total:
            raise ValueError(f"Truncated Ogg page body at offset {pos}")

        if verify_crc:
            calc_crc = ogg_crc32(ogg_bytes[pos:page_end])
            if calc_crc != page_crc:
                raise ValueError(
                    f"Ogg CRC mismatch at offset {pos}: got 0x{calc_crc:08X}, expected 0x{page_crc:08X}"
                )

        is_cont = (header_type & 0x01) != 0
        if cur_pkt and not is_cont:
            raise ValueError(f"Unexpected non-continued Ogg page while packet open at offset {pos}")

        seg_off = seg_table_end
        for seg_len in seg_table:
            cur_pkt.extend(ogg_bytes[seg_off : seg_off + seg_len])
            seg_off += seg_len
            if seg_len < 255:
                packets.append(bytes(cur_pkt))
                cur_pkt.clear()
                if max_packets is not None and len(packets) >= max_packets:
                    return packets

        pos = page_end

    if cur_pkt:
        raise ValueError("Unterminated Ogg packet at EOF")
    return packets


def parse_cassini_tags(ogg_bytes: bytes, verify_crc: bool = True) -> Dict[str, str]:
    """Extract and validate Cassini Vorbis comments from packet 1 (OpusTags)."""
    packets = parse_ogg_packets(ogg_bytes, verify_crc=verify_crc, max_packets=2)
    if len(packets) < 2:
        raise ValueError("Ogg stream has fewer than 2 packets (missing OpusTags)")
    if not packets[0].startswith(b"OpusHead"):
        raise ValueError("First Ogg packet is not OpusHead")
    tags_pkt = packets[1]
    if not tags_pkt.startswith(b"OpusTags"):
        raise ValueError("Second Ogg packet is not OpusTags")

    off = 8
    if off + 4 > len(tags_pkt):
        raise ValueError("Truncated OpusTags vendor length")
    vendor_len = struct.unpack_from("<I", tags_pkt, off)[0]
    off += 4 + vendor_len
    if off + 4 > len(tags_pkt):
        raise ValueError("Truncated OpusTags comment count")
    comment_count = struct.unpack_from("<I", tags_pkt, off)[0]
    off += 4

    tags: Dict[str, str] = {}
    for idx in range(comment_count):
        if off + 4 > len(tags_pkt):
            raise ValueError(f"Truncated OpusTags comment {idx} length")
        clen = struct.unpack_from("<I", tags_pkt, off)[0]
        off += 4
        if off + clen > len(tags_pkt):
            raise ValueError(f"Truncated OpusTags comment {idx} data")
        raw = tags_pkt[off : off + clen].decode("utf-8", errors="strict")
        off += clen
        if "=" not in raw:
            continue
        k, v = raw.split("=", 1)
        ku = k.strip().upper()
        is_load_bearing = (
            ku == "CASSINI_FORMAT"
            or ku.startswith("CASSINI_PAYLOAD_")
            or ku.startswith("CASSINI_TX_")
        )
        if is_load_bearing:
            if ku in tags:
                raise ValueError(f"Duplicate load-bearing Cassini tag: {ku}")
            tags[ku] = v
        elif ku not in tags:
            tags[ku] = v

    fmt = tags.get("CASSINI_FORMAT")
    if fmt != "org.cassini.portable-meeting/1":
        raise ValueError(f"Unexpected CASSINI_FORMAT: {fmt!r}")
    return tags


def _parse_strict_decimal(val: Optional[str], name: str) -> int:
    if val is None or not re.fullmatch(r"0|[1-9][0-9]*", val):
        raise ValueError(f"Malformed decimal tag {name}: {val!r}")
    num = int(val)
    if num > (1 << 53) - 1:
        raise ValueError(f"Out-of-range decimal tag {name}: {num}")
    return num


def decode_chunked_json_payload(
    tags: Dict[str, str],
    prefix: str,
    chunk_count: Optional[int] = None,
    expected_sha256: Optional[str] = None,
    expected_raw_bytes: Optional[int] = None,
    expected_encoding: str = "base64url+gzip+utf8json",
) -> Dict[str, Any]:
    """Reassemble ``<prefix>000..`` base64url+gzip+utf8json chunks and verify integrity."""
    prefix_u = prefix.upper()
    if expected_encoding != "base64url+gzip+utf8json":
        raise ValueError(f"Unsupported Cassini payload encoding: {expected_encoding}")

    # Collect matching numeric indices after prefix_u.
    chunk_pat = re.compile(rf"^{re.escape(prefix_u)}(\d+)$")
    found: Dict[int, str] = {}
    for k, v in tags.items():
        m = chunk_pat.match(k)
        if m:
            idx = int(m.group(1))
            if idx in found:
                raise ValueError(f"Duplicate chunk index {idx} for {prefix_u}")
            found[idx] = v

    if chunk_count is None:
        count_tag = tags.get(f"{prefix_u}CHUNK_COUNT") or tags.get(f"{prefix_u}COUNT") or tags.get(f"{prefix_u}CHUNKS")
        if count_tag is not None:
            chunk_count = _parse_strict_decimal(count_tag, f"{prefix_u}CHUNK_COUNT")
        else:
            chunk_count = len(found)

    if chunk_count <= 0:
        raise ValueError(f"Invalid chunk count ({chunk_count}) for prefix {prefix_u}")

    parts: List[bytes] = []
    for i in range(chunk_count):
        if i not in found:
            raise ValueError(f"Missing chunk {prefix_u}{i:03d}")
        cleaned = re.sub(r"[\x20\t\r\n]+", "", found[i]).encode("ascii")
        if not cleaned:
            raise ValueError(f"Empty chunk {prefix_u}{i:03d}")
        parts.append(cleaned)

    b64_data = b"".join(parts)
    if not _B64URL_RE.fullmatch(b64_data):
        raise ValueError(f"Invalid base64url characters in {prefix_u}")
    if b"=" not in b64_data:
        rem = len(b64_data) % 4
        if rem == 1:
            raise ValueError(f"Invalid base64url length in {prefix_u}")
        if rem:
            b64_data += b"=" * (4 - rem)
    elif len(b64_data) % 4 != 0:
        raise ValueError(f"Invalid padded base64url length in {prefix_u}")
    gz_bytes = base64.urlsafe_b64decode(b64_data)

    max_raw = (expected_raw_bytes if expected_raw_bytes is not None else 64 * 1024 * 1024)
    decomp = zlib.decompressobj(16 + zlib.MAX_WBITS)
    raw = decomp.decompress(gz_bytes, max_raw + 1)
    if len(raw) > max_raw or decomp.unconsumed_tail:
        raise ValueError(
            f"Decompressed payload for {prefix_u} exceeds limit ({max_raw} bytes)"
        )
    raw += decomp.flush()
    if not decomp.eof or decomp.unused_data:
        raise ValueError(f"Malformed or multi-member gzip stream for {prefix_u}")
    if expected_raw_bytes is not None and len(raw) != expected_raw_bytes:
        raise ValueError(
            f"Raw byte size mismatch for {prefix_u}: got {len(raw)}, expected {expected_raw_bytes}"
        )

    actual_sha = hashlib.sha256(raw).hexdigest().lower()
    if expected_sha256 is not None and actual_sha != expected_sha256.lower():
        raise ValueError(
            f"SHA-256 mismatch for {prefix_u}: got {actual_sha}, expected {expected_sha256}"
        )

    obj = json.loads(raw.decode("utf-8"))
    if not isinstance(obj, dict):
        raise ValueError(f"Decoded payload for {prefix_u} is not a JSON object")
    return obj


def load_cassini_document(
    ogg_path: Path,
    transcript_id: Optional[str] = None,
    verify_crc: bool = True,
) -> Tuple[Dict[str, Any], Dict[str, Any], Dict[str, str]]:
    """Load Cassini manifest + selected transcript from an Ogg Opus container."""
    ogg_bytes = ogg_path.read_bytes()
    tags = parse_cassini_tags(ogg_bytes, verify_crc=verify_crc)

    manifest_enc = tags.get("CASSINI_PAYLOAD_ENCODING")
    if manifest_enc != "base64url+gzip+utf8json":
        raise ValueError(f"Missing or unsupported CASSINI_PAYLOAD_ENCODING: {manifest_enc!r}")

    cnt_tag = tags.get("CASSINI_PAYLOAD_CHUNK_COUNT")
    if cnt_tag is None:
        raise ValueError("Missing required CASSINI_PAYLOAD_CHUNK_COUNT tag")
    manifest_cnt = _parse_strict_decimal(cnt_tag, "CASSINI_PAYLOAD_CHUNK_COUNT")
    if manifest_cnt <= 0:
        raise ValueError(f"CASSINI_PAYLOAD_CHUNK_COUNT must be >= 1, got {manifest_cnt}")

    manifest_sha = tags.get("CASSINI_PAYLOAD_SHA256")
    if not manifest_sha or not re.fullmatch(r"[0-9a-f]{64}", manifest_sha):
        raise ValueError(f"Missing or invalid CASSINI_PAYLOAD_SHA256: {manifest_sha!r}")

    manifest_raw_s = tags.get("CASSINI_PAYLOAD_RAW_BYTES")
    manifest_raw = (
        _parse_strict_decimal(manifest_raw_s, "CASSINI_PAYLOAD_RAW_BYTES")
        if manifest_raw_s is not None
        else None
    )

    manifest = decode_chunked_json_payload(
        tags,
        prefix="CASSINI_PAYLOAD_",
        chunk_count=manifest_cnt,
        expected_sha256=manifest_sha,
        expected_raw_bytes=manifest_raw,
        expected_encoding=manifest_enc,
    )

    transcripts = manifest.get("transcripts") or []
    if not transcripts:
        raise ValueError("Cassini manifest contains no transcripts")

    chosen = None
    if transcript_id is not None:
        for entry in transcripts:
            if entry.get("id") == transcript_id:
                chosen = entry
                break
        if chosen is None:
            avail = [str(t.get("id")) for t in transcripts]
            raise ValueError(f"Transcript {transcript_id!r} not found (available: {avail})")
    else:
        for entry in transcripts:
            if entry.get("default") is True:
                chosen = entry
                break
        if chosen is None and tags.get("CASSINI_TRANSCRIPT_DEFAULT"):
            wanted_id = tags["CASSINI_TRANSCRIPT_DEFAULT"]
            for entry in transcripts:
                if entry.get("id") == wanted_id:
                    chosen = entry
                    break
        if chosen is None:
            chosen = transcripts[0]

    pref = chosen.get("payloadRef")
    if not isinstance(pref, dict):
        raise ValueError(f"Transcript {chosen.get('id')!r} missing payloadRef")

    prefix = str(pref["prefix"])
    chunk_count = int(pref["chunkCount"]) if "chunkCount" in pref else None
    sha256 = str(pref["sha256"]) if "sha256" in pref else None
    raw_bytes = int(pref["rawBytes"]) if "rawBytes" in pref else None
    encoding = str(pref.get("encoding", "base64url+gzip+utf8json"))

    tx_body = decode_chunked_json_payload(
        tags,
        prefix=prefix,
        chunk_count=chunk_count,
        expected_sha256=sha256,
        expected_raw_bytes=raw_bytes,
        expected_encoding=encoding,
    )
    return manifest, tx_body, tags


def sanitize_display_text(text: str, max_bytes: int) -> bytes:
    """Normalize punctuation for clean 6x8 LCD display and clamp UTF-8 byte length."""
    norm = text.translate(_TYPO_TRANSLATION)
    norm = re.sub(r"[\x00-\x1F\x7F]+", " ", norm).strip()
    raw = norm.encode("utf-8")
    if len(raw) <= max_bytes:
        return raw
    # Back off to avoid splitting a multi-byte UTF-8 character.
    cut = max_bytes
    while cut > 0 and (raw[cut] & 0xC0) == 0x80:
        cut -= 1
    return raw[:cut]


def _parse_hex_rgb(val: Any) -> Optional[Tuple[int, int, int]]:
    if not isinstance(val, str):
        return None
    s = val.strip()
    if s.startswith("#") and len(s) == 7:
        try:
            return int(s[1:3], 16), int(s[3:5], 16), int(s[5:7], 16)
        except ValueError:
            return None
    return None


def build_cwx_bytes(
    manifest: Dict[str, Any],
    transcript: Dict[str, Any],
    fallback_title: str = "Meeting",
) -> bytes:
    """Serialize Cassini manifest + transcript into a CWX1 binary payload.

    CRITICAL: ``transcript["items"]`` is iterated in exact speaker-turn file order
    and is NEVER sorted by ``startMs``.
    """
    raw_title = (
        manifest.get("title")
        or (manifest.get("meeting") or {}).get("title")
        or fallback_title
    )
    title_bytes = sanitize_display_text(str(raw_title), CWX_MAX_TITLE_BYTES)

    # Collect declared speakers from manifest.
    speakers_meta: List[Dict[str, Any]] = list(manifest.get("speakers") or [])
    speaker_id_to_idx: Dict[str, int] = {}
    speaker_defs: List[Tuple[int, bytes]] = []

    def add_speaker(sid: Optional[str], label: str, rgb_override: Optional[Tuple[int, int, int]] = None) -> int:
        if sid is not None and sid in speaker_id_to_idx:
            return speaker_id_to_idx[sid]
        if len(speaker_defs) >= CWX_MAX_SPEAKERS:
            return CWX_MAX_SPEAKERS - 1
        idx = len(speaker_defs)
        rgb = rgb_override or DEFAULT_SPEAKER_PALETTE_RGB[idx % len(DEFAULT_SPEAKER_PALETTE_RGB)]
        lbl_bytes = sanitize_display_text(label or f"Speaker {idx + 1}", CWX_MAX_LABEL_BYTES)
        speaker_defs.append((rgb565(*rgb), lbl_bytes))
        if sid is not None:
            speaker_id_to_idx[sid] = idx
        return idx

    # Reserve at most 15 declared speakers upfront so Unknown fits if needed.
    for sp in speakers_meta:
        sid = sp.get("id")
        if not sid:
            continue
        lbl = sp.get("label") or sp.get("name") or str(sid)
        rgb = _parse_hex_rgb(sp.get("color"))
        if len(speaker_defs) < CWX_MAX_SPEAKERS - 1:
            add_speaker(str(sid), str(lbl), rgb)

    unknown_idx: Optional[int] = None

    def resolve_speaker(sid: Any) -> int:
        nonlocal unknown_idx
        key = str(sid) if sid is not None else ""
        if key and key in speaker_id_to_idx:
            return speaker_id_to_idx[key]
        if unknown_idx is None:
            unknown_idx = add_speaker(None, "Unknown", (210, 210, 210))
        return unknown_idx

    items = transcript.get("items") or []
    words_buf = bytearray()
    word_offsets: List[int] = []
    word_ends: List[int] = []
    max_end_ms = 0
    prev_spk: Optional[int] = None

    for item in items:
        raw_text = str(item.get("text", ""))
        text_bytes = sanitize_display_text(raw_text, CWX_MAX_WORD_BYTES)
        if not text_bytes:
            continue

        start_ms = max(0, int(round(float(item.get("startMs", 0)))))
        end_ms = max(start_ms, int(round(float(item.get("endMs", start_ms)))))
        if end_ms > max_end_ms:
            max_end_ms = end_ms

        spk_idx = resolve_speaker(item.get("speaker"))
        flags = 0
        if prev_spk is None or spk_idx != prev_spk or item.get("turnStart"):
            flags |= CWX_FLAG_TURN_START
        if item.get("lowConfidenceSpeaker") or item.get("lowConfidence"):
            flags |= CWX_FLAG_LOW_CONF
        prev_spk = spk_idx

        word_offsets.append(len(words_buf))
        word_ends.append(end_ms)
        words_buf.extend(
            struct.pack("<IIBBB", start_ms, end_ms, spk_idx & 0xFF, flags & 0xFF, len(text_bytes))
        )
        words_buf.extend(text_bytes)

    if not speaker_defs:
        add_speaker(None, "Speaker 1")

    dur_val = (
        (manifest.get("meeting") or {}).get("durationMs")
        or (manifest.get("audio") or {}).get("durationMs")
        or manifest.get("durationMs")
        or max_end_ms
        or 0
    )
    duration_ms = int(round(float(dur_val)))
    if max_end_ms > duration_ms:
        duration_ms = max_end_ms

    word_count = len(word_offsets)
    speaker_count = len(speaker_defs)
    seek_count = max(1, (duration_ms // 1000) + 1)

    speakers_off = 64
    seek_off = speakers_off + speaker_count * 24
    words_off = seek_off + seek_count * 8

    # Build monotonic 1 Hz seek table tracking earliest non-finished word at sec*1000.
    seek_bytes = bytearray()
    scan_idx = 0
    total_words_size = len(words_buf)
    for sec in range(seek_count):
        target_ms = sec * 1000
        while scan_idx < word_count and word_ends[scan_idx] < target_ms:
            scan_idx += 1
        rel_off = word_offsets[scan_idx] if scan_idx < word_count else total_words_size
        seek_bytes.extend(struct.pack("<II", rel_off, scan_idx))

    speakers_bytes = bytearray()
    for col565, lbl in speaker_defs:
        lbl_padded = lbl.ljust(20, b"\x00")[:20]
        speakers_bytes.extend(struct.pack("<HBB20s", col565, 0, 0, lbl_padded))

    title_padded = title_bytes.ljust(32, b"\x00")[:32]
    hdr = struct.pack(
        "<4sHHIIIIII32s",
        CWX_MAGIC,
        CWX_VERSION,
        speaker_count,
        duration_ms,
        word_count,
        speakers_off,
        seek_off,
        seek_count,
        words_off,
        title_padded,
    )
    assert len(hdr) == 64
    return hdr + bytes(speakers_bytes) + bytes(seek_bytes) + bytes(words_buf)


def encode_ima_adpcm_mono(samples: Sequence[int]) -> Tuple[bytes, int]:
    """Encode 16-bit mono PCM samples into Microsoft IMA-ADPCM 256-byte blocks.

    Returns ``(adpcm_bytes, padded_or_original_sample_count)`` where the caller writes
    ``fact`` with the true unpadded sample count ``len(samples)``.
    """
    n_samples = len(samples)
    if n_samples == 0:
        samples = [0]
        n_samples = 1

    out = bytearray()
    step_idx = 0
    if n_samples >= 2:
        init_delta = abs(int(samples[1]) - int(samples[0]))
        while step_idx < 88 and IMA_STEP_TABLE[step_idx] < init_delta:
            step_idx += 1
    pos = 0

    def encode_nibble(target: int, predictor: int, sidx: int) -> Tuple[int, int, int]:
        step = IMA_STEP_TABLE[sidx]
        diff = target - predictor
        code = 0
        if diff < 0:
            code = 8
            diff = -diff
        if diff >= step:
            code |= 4
            diff -= step
        if diff >= (step >> 1):
            code |= 2
            diff -= (step >> 1)
        if diff >= (step >> 2):
            code |= 1

        # Exact decoder reconstruction keeps predictor in sync.
        diffq = step >> 3
        if code & 4:
            diffq += step
        if code & 2:
            diffq += step >> 1
        if code & 1:
            diffq += step >> 2
        if code & 8:
            predictor -= diffq
        else:
            predictor += diffq
        if predictor > 32767:
            predictor = 32767
        elif predictor < -32768:
            predictor = -32768

        sidx += IMA_INDEX_TABLE[code]
        if sidx < 0:
            sidx = 0
        elif sidx > 88:
            sidx = 88
        return code, predictor, sidx

    while pos < n_samples:
        samp0 = int(samples[pos])
        if samp0 > 32767:
            samp0 = 32767
        elif samp0 < -32768:
            samp0 = -32768
        predictor = samp0
        out.extend(struct.pack("<hBB", samp0, step_idx, 0))
        pos += 1

        # Remaining 504 samples pack into 252 bytes (2 nibbles/byte).
        for byte_i in range(IMA_BLOCK_ALIGN - 4):
            s_lo = int(samples[pos]) if pos < n_samples else samp0
            pos += 1
            c_lo, predictor, step_idx = encode_nibble(s_lo, predictor, step_idx)

            s_hi = int(samples[pos]) if pos < n_samples else s_lo
            pos += 1
            c_hi, predictor, step_idx = encode_nibble(s_hi, predictor, step_idx)

            out.append((c_lo & 0x0F) | ((c_hi & 0x0F) << 4))

    return bytes(out), n_samples


def write_pcm_wav(path: Path, samples: Sequence[int], sample_rate: int = SAMPLE_RATE_HZ) -> None:
    arr = array.array("h", samples)
    if sys.byteorder != "little":
        arr.byteswap()
    pcm_bytes = arr.tobytes()
    data_size = len(pcm_bytes)
    riff_size = 4 + 24 + 8 + data_size
    hdr = struct.pack(
        "<4sI4s4sIHHIIHH4sI",
        b"RIFF",
        riff_size,
        b"WAVE",
        b"fmt ",
        16,
        1,              # PCM
        1,              # mono
        sample_rate,
        sample_rate * 2,
        2,              # blockAlign
        16,             # bitsPerSample
        b"data",
        data_size,
    )
    path.write_bytes(hdr + pcm_bytes)


def write_ima_adpcm_wav(path: Path, samples: Sequence[int], sample_rate: int = SAMPLE_RATE_HZ) -> None:
    adpcm_bytes, true_samples = encode_ima_adpcm_mono(samples)
    byte_rate = (sample_rate * IMA_BLOCK_ALIGN) // IMA_SAMPLES_PER_BLOCK
    # RIFF(12) + fmt(28) + fact(12) + data_hdr(8) + adpcm_bytes
    fmt_chunk = struct.pack(
        "<4sIHHIIHHHH",
        b"fmt ",
        20,
        0x0011,             # WAVE_FORMAT_DVI_ADPCM / IMA_ADPCM
        1,                  # mono
        sample_rate,
        byte_rate,
        IMA_BLOCK_ALIGN,    # 256
        4,                  # bitsPerSample
        2,                  # cbSize
        IMA_SAMPLES_PER_BLOCK,  # 505
    )
    fact_chunk = struct.pack("<4sII", b"fact", 4, true_samples)
    data_hdr = struct.pack("<4sI", b"data", len(adpcm_bytes))
    riff_size = 4 + len(fmt_chunk) + len(fact_chunk) + len(data_hdr) + len(adpcm_bytes)
    riff_hdr = struct.pack("<4sI4s", b"RIFF", riff_size, b"WAVE")
    path.write_bytes(riff_hdr + fmt_chunk + fact_chunk + data_hdr + adpcm_bytes)


def read_wav_mono_16k(wav_path: Path) -> List[int]:
    """Read a mono 16-bit 16 kHz PCM WAV file into int16 samples."""
    raw = wav_path.read_bytes()
    if len(raw) < 44 or raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise ValueError(f"Unsupported WAV header in {wav_path}")
    off = 12
    fmt_info = None
    data_bytes = b""
    while off + 8 <= len(raw):
        cid = raw[off : off + 4]
        csz = struct.unpack_from("<I", raw, off + 4)[0]
        body = raw[off + 8 : off + 8 + csz]
        if cid == b"fmt ":
            fmt_info = struct.unpack_from("<HHIIHH", body, 0)
        elif cid == b"data":
            data_bytes = body
            break
        off += 8 + csz + (csz & 1)
    if fmt_info is None:
        raise ValueError(f"Missing fmt chunk in {wav_path}")
    audio_fmt, channels, sr, _, _, bits = fmt_info
    if audio_fmt != 1 or channels != 1 or sr != SAMPLE_RATE_HZ or bits != 16:
        raise ValueError(
            f"Expected 16 kHz mono 16-bit PCM WAV, got fmt={audio_fmt} ch={channels} sr={sr} bits={bits}"
        )
    arr = array.array("h")
    arr.frombytes(data_bytes[: len(data_bytes) & ~1])
    if sys.byteorder != "little":
        arr.byteswap()
    return list(arr)


def decode_audio_with_ffmpeg(input_path: Path) -> List[int]:
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise RuntimeError(
            "ffmpeg not found on PATH; pass --wav-in <16k-mono.wav> or install ffmpeg"
        )
    cmd = [
        ffmpeg,
        "-v", "error",
        "-i", str(input_path),
        "-ac", "1",
        "-ar", str(SAMPLE_RATE_HZ),
        "-f", "s16le",
        "-",
    ]
    res = subprocess.run(cmd, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    pcm = res.stdout[: len(res.stdout) & ~1]
    arr = array.array("h")
    arr.frombytes(pcm)
    if sys.byteorder != "little":
        arr.byteswap()
    return list(arr)


def _derive_stem(input_path: Path) -> str:
    name = input_path.name
    for suffix in (".cassini.opus", ".cassini.ogg", ".opus", ".ogg"):
        if name.lower().endswith(suffix):
            name = name[: -len(suffix)]
            break
    else:
        name = input_path.stem
    safe = re.sub(r"[^A-Za-z0-9._-]+", "_", name).strip("._") or "cassini"
    return safe


def convert_cassini(
    input_path: Path,
    output_dir: Optional[Path] = None,
    transcript_id: Optional[str] = None,
    pcm: bool = False,
    wav_in: Optional[Path] = None,
    cwx_only: bool = False,
) -> Tuple[Optional[Path], Path]:
    manifest, transcript, _ = load_cassini_document(input_path, transcript_id=transcript_id)
    out_dir = output_dir or input_path.parent
    out_dir.mkdir(parents=True, exist_ok=True)
    stem = _derive_stem(input_path)

    cwx_path = out_dir / f"{stem}.cwx"
    cwx_bytes = build_cwx_bytes(manifest, transcript, fallback_title=stem)
    cwx_path.write_bytes(cwx_bytes)

    wav_path: Optional[Path] = None
    if not cwx_only:
        wav_path = out_dir / f"{stem}.wav"
        if wav_in is not None:
            samples = read_wav_mono_16k(wav_in)
        else:
            samples = decode_audio_with_ffmpeg(input_path)
        if pcm:
            write_pcm_wav(wav_path, samples, SAMPLE_RATE_HZ)
        else:
            write_ima_adpcm_wav(wav_path, samples, SAMPLE_RATE_HZ)

    return wav_path, cwx_path


# ---------------------------------------------------------------------------
# Self-test harness (multi-page Ogg + turn-order + C firmware interop check)
# ---------------------------------------------------------------------------

def _build_ogg_page(
    packets_segments: List[Tuple[bytes, bool]],
    serial: int,
    page_seq: int,
    continued: bool = False,
    bos: bool = False,
    eos: bool = False,
    granule: int = 0,
) -> bytes:
    """Build one canonical Ogg page with valid CRC."""
    seg_table = bytearray()
    body = bytearray()
    for chunk, terminates in packets_segments:
        # Each 255-byte lace continues; final lace < 255 terminates.
        off = 0
        while len(chunk) - off >= 255:
            seg_table.append(255)
            body.extend(chunk[off : off + 255])
            off += 255
        rem = chunk[off:]
        if terminates:
            seg_table.append(len(rem))
            body.extend(rem)
        else:
            # Must be exact multiple of 255 to continue onto next page.
            if rem:
                raise ValueError("Non-terminating segment must be multiple of 255 bytes")

    htype = (0x01 if continued else 0) | (0x02 if bos else 0) | (0x04 if eos else 0)
    hdr = struct.pack(
        "<4sBBqIIIB",
        b"OggS",
        0,
        htype,
        granule,
        serial,
        page_seq,
        0,
        len(seg_table),
    ) + bytes(seg_table) + bytes(body)
    crc = ogg_crc32(hdr)
    buf = bytearray(hdr)
    struct.pack_into("<I", buf, 22, crc)
    return bytes(buf)


def _encode_cassini_payload_chunks(obj: Dict[str, Any], chunk_size: int = 60) -> Tuple[List[str], str, int, int]:
    raw = json.dumps(obj, separators=(",", ":"), sort_keys=True).encode("utf-8")
    sha = hashlib.sha256(raw).hexdigest()
    gz = gzip.compress(raw, mtime=0)
    b64 = base64.urlsafe_b64encode(gz).decode("ascii").rstrip("=")
    chunks = [b64[i : i + chunk_size] for i in range(0, len(b64), chunk_size)]
    return chunks, sha, len(raw), len(gz)


def _build_synthetic_cassini_ogg(
    manifest: Dict[str, Any],
    transcript: Dict[str, Any],
    extra_tags: Sequence[Tuple[str, str]] = (),
    manifest_chunk_count_override: Optional[int] = None,
) -> bytes:
    tx_chunks, tx_sha, tx_len, tx_gz_len = _encode_cassini_payload_chunks(transcript, chunk_size=48)
    # Patch manifest payloadRef accurately before encoding manifest!
    manifest = json.loads(json.dumps(manifest))
    manifest["transcripts"][0]["payloadRef"] = {
        "prefix": "CASSINI_TX_ASR_MAIN_PAYLOAD_",
        "chunkCount": len(tx_chunks),
        "mime": "application/vnd.cassini.transcript-words+json",
        "encoding": "base64url+gzip+utf8json",
        "sha256": tx_sha,
        "rawBytes": tx_len,
        "gzipBytes": tx_gz_len,
    }
    m_chunks, m_sha, m_len, m_gz_len = _encode_cassini_payload_chunks(manifest, chunk_size=50)
    declared_m_cnt = manifest_chunk_count_override if manifest_chunk_count_override is not None else len(m_chunks)

    comments: List[str] = [
        "cassini_format=org.cassini.portable-meeting/1",  # intentionally lowercase to verify case-insensitivity
        "CASSINI_PROFILE=ogg-opus",
        "CASSINI_PAYLOAD_MIME=application/vnd.cassini.portable-meeting+json",
        "CASSINI_PAYLOAD_ENCODING=base64url+gzip+utf8json",
        f"CASSINI_PAYLOAD_CHUNK_COUNT={declared_m_cnt}",
        f"CASSINI_PAYLOAD_SHA256={m_sha}",
        f"CASSINI_PAYLOAD_RAW_BYTES={m_len}",
        f"CASSINI_PAYLOAD_GZIP_BYTES={m_gz_len}",
        f"CASSINI_TRANSCRIPT_IDS={manifest['transcripts'][0]['id']}",
        f"CASSINI_TRANSCRIPT_DEFAULT={manifest['transcripts'][0]['id']}",
        "CASSINI_TX_ASR_MAIN_PAYLOAD_MIME=application/vnd.cassini.transcript-words+json",
        "CASSINI_TX_ASR_MAIN_PAYLOAD_ENCODING=base64url+gzip+utf8json",
        f"CASSINI_TX_ASR_MAIN_PAYLOAD_CHUNK_COUNT={len(tx_chunks)}",
        f"CASSINI_TX_ASR_MAIN_PAYLOAD_SHA256={tx_sha}",
        f"CASSINI_TX_ASR_MAIN_PAYLOAD_RAW_BYTES={tx_len}",
        f"CASSINI_TX_ASR_MAIN_PAYLOAD_GZIP_BYTES={tx_gz_len}",
    ]
    for i, c in enumerate(m_chunks):
        comments.append(f"CASSINI_PAYLOAD_{i:03d}={c}")
    for i, c in enumerate(tx_chunks):
        comments.append(f"CASSINI_TX_ASR_MAIN_PAYLOAD_{i:03d}={c}")
    for k, v in extra_tags:
        comments.append(f"{k}={v}")

    opus_head = b"OpusHead" + struct.pack("<BBHIhB", 1, 1, 312, 16000, 0, 0)
    vendor = b"cassini-test"
    tags_buf = bytearray(b"OpusTags")
    tags_buf.extend(struct.pack("<I", len(vendor)))
    tags_buf.extend(vendor)
    tags_buf.extend(struct.pack("<I", len(comments)))
    for c in comments:
        cb = c.encode("utf-8")
        tags_buf.extend(struct.pack("<I", len(cb)))
        tags_buf.extend(cb)

    # Split OpusTags across TWO Ogg pages if large enough (255*2 on page 1, rest on page 2).
    split_at = min(len(tags_buf), 255 * 2)
    # Ensure non-terminating slice is multiple of 255.
    split_at = (split_at // 255) * 255

    serial = 0x51525354
    p0 = _build_ogg_page([(opus_head, True)], serial, 0, bos=True, granule=0)
    if split_at > 0 and split_at < len(tags_buf):
        p1 = _build_ogg_page([(bytes(tags_buf[:split_at]), False)], serial, 1, granule=0)
        p2 = _build_ogg_page([(bytes(tags_buf[split_at:]), True)], serial, 2, continued=True, granule=0)
        p3 = _build_ogg_page([(b"\xf8\xff\xfe", True)], serial, 3, eos=True, granule=192000)
        return p0 + p1 + p2 + p3
    p1 = _build_ogg_page([(bytes(tags_buf), True)], serial, 1, granule=0)
    p2 = _build_ogg_page([(b"\xf8\xff\xfe", True)], serial, 2, eos=True, granule=192000)
    return p0 + p1 + p2


def run_self_test() -> int:
    # 1. Overlapping speaker turn test: S0 starts at 1600ms, S1 overlaps earlier at 1200ms!
    # We MUST keep [S0: "alpha", S0: "beta", S1: "overlap"] order, NOT sort by startMs.
    manifest: Dict[str, Any] = {
        "kind": "cassini-portable-meeting",
        "version": 1,
        "profile": "ogg-opus",
        "meeting": {
            "id": "mtg_test01",
            "title": "Design Review \u2014 Q4",
            "createdAtUtc": "2026-09-02T10:00:00Z",
            "durationMs": 4000,
        },
        "audio": {
            "container": "ogg",
            "codec": "opus",
            "sampleRate": 48000,
            "channels": 1,
            "sampleCount": 192000,
            "durationMs": 4000,
        },
        "integrity": {
            "matchPolicy": "exact-opus-audio-v1",
            "opusAudioSha256": "0" * 64,
            "sampleRate": 48000,
            "channels": 1,
            "sampleCount": 192000,
            "durationMs": 4000,
        },
        "speakers": [
            {"id": "spk_0", "label": "Alice"},
            {"id": "spk_1", "label": "Bob"},
        ],
        "transcripts": [{"id": "asr_main", "default": True, "format": "cassini.words.v1", "wordCount": 4, "payloadRef": {}}],
    }
    transcript: Dict[str, Any] = {
        "format": "cassini.words.v1",
        "wordCount": 4,
        "items": [
            {"speaker": "spk_0", "startMs": 100, "endMs": 600, "text": "Hello"},
            {"speaker": "spk_0", "startMs": 1600, "endMs": 2600, "text": "everyone\u2019s"},
            {"speaker": "spk_1", "startMs": 1200, "endMs": 1800, "text": "Wait!", "lowConfidenceSpeaker": True},
            {"speaker": "spk_missing", "startMs": 2700, "endMs": 3200, "text": "Mystery"},
        ],
    }

    ogg_ok = _build_synthetic_cassini_ogg(
        manifest, transcript, extra_tags=[("CASSINI_PAYLOAD_099", "stale_ignored")]
    )

    with tempfile.TemporaryDirectory(prefix="cassini_selftest_") as td:
        tmp = Path(td)
        ogg_path = tmp / "meeting_demo.cassini.opus"
        ogg_path.write_bytes(ogg_ok)

        # Generate 4 seconds of 440 Hz sine wave PCM for realistic IMA-ADPCM SNR check.
        pcm_in = [
            int(14000.0 * math.sin(2.0 * math.pi * 440.0 * i / SAMPLE_RATE_HZ))
            for i in range(SAMPLE_RATE_HZ * 4)
        ]
        wav_in_path = tmp / "source_16k.wav"
        write_pcm_wav(wav_in_path, pcm_in)

        wav_out, cwx_out = convert_cassini(
            ogg_path, output_dir=tmp / "out", wav_in=wav_in_path
        )
        assert wav_out is not None and wav_out.exists()
        assert cwx_out.exists()

        # Verify CWX binary structure in Python.
        cwx_data = cwx_out.read_bytes()
        magic, ver, spk_cnt, dur_ms, word_cnt, spk_off, seek_off, seek_cnt, words_off, title_raw = struct.unpack_from(
            "<4sHHIIIIII32s", cwx_data, 0
        )
        assert magic == b"CWX1" and ver == 1
        assert spk_cnt == 3  # Alice, Bob, Unknown
        assert dur_ms == 4000
        assert word_cnt == 4
        assert title_raw.split(b"\x00", 1)[0] == b"Design Review - Q4"
        assert seek_cnt == 5

        # Verify duplicate load-bearing tag rejection.
        ogg_dup = _build_synthetic_cassini_ogg(
            manifest, transcript, extra_tags=[("CASSINI_FORMAT", "org.cassini.portable-meeting/1")]
        )
        try:
            parse_cassini_tags(ogg_dup)
            raise AssertionError("Duplicate load-bearing tag was not rejected")
        except ValueError as e:
            assert "Duplicate load-bearing" in str(e)

        # Verify missing manifest chunk (e.g., CHUNK_COUNT higher than available chunks) rejection.
        ogg_missing_chunk = tmp / "missing_chunk.cassini.opus"
        ogg_missing_chunk.write_bytes(
            _build_synthetic_cassini_ogg(manifest, transcript, manifest_chunk_count_override=99)
        )
        try:
            load_cassini_document(ogg_missing_chunk)
            raise AssertionError("Missing manifest chunk was not rejected")
        except ValueError as e:
            assert "Missing chunk" in str(e)

        # Verify SHA-256 mismatch rejection.
        try:
            decode_chunked_json_payload(
                parse_cassini_tags(ogg_ok),
                prefix="CASSINI_PAYLOAD_",
                expected_sha256="0" * 64,
            )
            raise AssertionError("Bad SHA-256 was not rejected")
        except ValueError as e:
            assert "SHA-256 mismatch" in str(e)

        # Compile and run real C interop test against open_fw/src/wav.c & open_fw/src/cwx.c!
        repo_root = Path(__file__).resolve().parent.parent
        c_test_src = tmp / "cwx_wav_interop.c"
        c_test_bin = tmp / "cwx_wav_interop"
        c_test_src.write_text(
            r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "openfw_wav.h"
#include "openfw_cwx.h"

typedef struct {
    FILE *fp;
} file_stream_t;

static int fs_cwx_read(void *user, uint32_t offset, void *dst, uint32_t len, uint32_t *out_read) {
    file_stream_t *fs = (file_stream_t *)user;
    if (fseek(fs->fp, (long)offset, SEEK_SET) != 0) return -1;
    size_t got = fread(dst, 1, len, fs->fp);
    if (out_read) *out_read = (uint32_t)got;
    return 0;
}

int main(int argc, char **argv) {
    assert(argc == 3);
    FILE *fw = fopen(argv[1], "rb");
    FILE *fc = fopen(argv[2], "rb");
    assert(fw && fc);

    uint8_t wav_hdr[256];
    size_t hdr_len = fread(wav_hdr, 1, sizeof(wav_hdr), fw);
    assert(hdr_len >= 60);

    wav_stream_info_t wi;
    assert(wav_parse_stream(wav_hdr, (uint32_t)hdr_len, &wi) == 0);
    assert(wi.format_tag == WAV_FMT_IMA_ADPCM);
    assert(wi.channels == 1);
    assert(wi.sample_rate == 16000);
    assert(wi.block_align == 256);
    assert(wi.samples_per_block == 505);

    /* Decode first 8 blocks (~4040 samples) and measure SNR against 440 Hz reference. */
    uint8_t blk[256];
    int16_t pcm[505];
    double err_e = 0.0, sig_e = 0.0;
    size_t sample_idx = 0;
    for (int b = 0; b < 8; b++) {
        assert(fseek(fw, (long)(wi.data_offset + (uint32_t)(b * 256)), SEEK_SET) == 0);
        size_t got = fread(blk, 1, 256, fw);
        assert(got == 256);
        int n = wav_decode_ima_adpcm_block(blk, 256, 1, pcm, 505);
        assert(n == 505);
        for (int i = 0; i < n; i++, sample_idx++) {
            double ref = 14000.0 * sin(2.0 * 3.14159265358979323846 * 440.0 * (double)sample_idx / 16000.0);
            double diff = (double)pcm[i] - ref;
            sig_e += ref * ref;
            err_e += diff * diff;
        }
    }
    double snr_db = 10.0 * log10(sig_e / (err_e + 1e-9));
    assert(snr_db > 30.0);

    assert(fseek(fc, 0, SEEK_END) == 0);
    long cwx_sz = ftell(fc);
    assert(cwx_sz > 64);

    file_stream_t cfs = { fc };
    cwx_stream_t cs;
    assert(cwx_open(&cs, fs_cwx_read, &cfs, (uint32_t)cwx_sz) == 0);
    assert(cs.speaker_count == 3);
    assert(cs.word_count == 4);
    assert(cs.duration_ms == 4000);
    assert(strcmp(cs.title, "Design Review - Q4") == 0);
    assert(strcmp(cwx_speaker_label(&cs, 0), "Alice") == 0);
    assert(strcmp(cwx_speaker_label(&cs, 1), "Bob") == 0);
    assert(strcmp(cwx_speaker_label(&cs, 2), "Unknown") == 0);

    cwx_word_t words[8];
    int active_idx = -1;
    int nw = cwx_read_window(&cs, 1700, words, 8, &active_idx);
    assert(nw == 4);
    /* Verify EXACT turn order even though Wait! starts before everyone's! */
    assert(strcmp(words[0].text, "Hello") == 0 && (words[0].flags & CWX_FLAG_TURN_START));
    assert(strcmp(words[1].text, "everyone's") == 0 && !(words[1].flags & CWX_FLAG_TURN_START));
    assert(words[1].start_ms == 1600);
    assert(strcmp(words[2].text, "Wait!") == 0 && (words[2].flags & CWX_FLAG_TURN_START) && (words[2].flags & CWX_FLAG_LOW_CONF));
    assert(words[2].start_ms == 1200);
    assert(strcmp(words[3].text, "Mystery") == 0 && words[3].speaker_idx == 2);
    assert(active_idx == 1 || active_idx == 2);

    printf("C_INTEROP_OK snr=%.1fdB words=%d active=%d\n", snr_db, nw, active_idx);
    fclose(fw);
    fclose(fc);
    return 0;
}
'''
        )

        cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        if not cc:
            raise RuntimeError("No host C compiler found for CWX/WAV interop verification")
        subprocess.run(
            [
                cc,
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{repo_root / 'open_fw' / 'include'}",
                str(c_test_src),
                str(repo_root / "open_fw" / "src" / "wav.c"),
                str(repo_root / "open_fw" / "src" / "cwx.c"),
                "-lm",
                "-o",
                str(c_test_bin),
            ],
            check=True,
        )
        res = subprocess.run(
            [str(c_test_bin), str(wav_out), str(cwx_out)],
            check=True,
            stdout=subprocess.PIPE,
            text=True,
        )
        line = res.stdout.strip()
        assert "C_INTEROP_OK" in line, f"Unexpected C test output: {line}"
        print(f"PASSED ({line})")
    return 0


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("input", nargs="?", type=Path, help="Input .cassini.opus / .ogg file")
    ap.add_argument("-o", "--output-dir", type=Path, default=None, help="Output directory")
    ap.add_argument("--transcript", default=None, help="Transcript ID to export")
    ap.add_argument("--pcm", action="store_true", help="Write 16-bit PCM WAV instead of IMA-ADPCM")
    ap.add_argument("--wav-in", type=Path, default=None, help="Pre-decoded 16 kHz mono WAV audio source")
    ap.add_argument("--cwx-only", action="store_true", help="Write only .cwx (skip .wav)")
    ap.add_argument("--self-test", action="store_true", help="Run built-in verification suite")
    args = ap.parse_args(argv)

    if args.self_test:
        return run_self_test()
    if args.input is None:
        ap.error("input file is required unless --self-test is specified")

    wav_path, cwx_path = convert_cassini(
        args.input,
        output_dir=args.output_dir,
        transcript_id=args.transcript,
        pcm=args.pcm,
        wav_in=args.wav_in,
        cwx_only=args.cwx_only,
    )
    if wav_path is not None:
        print(f"Wrote {wav_path} ({wav_path.stat().st_size} bytes)")
    print(f"Wrote {cwx_path} ({cwx_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
