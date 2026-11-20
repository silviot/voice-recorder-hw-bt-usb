#!/usr/bin/env python3
"""
Generate packed 4-bit (16-level) grayscale anti-aliased proportional C font
tables for Cassini OpenFW (SL6801 ST7735S 160x80 RGB565 display).

Features:
  - Printable ASCII (0x20..0x7E, 95 glyphs)
  - 5 UI typography glyphs (0x7F..0x83 internal slots):
      0x7F: U+00B7 middle dot   (·)
      0x80: U+00D7 multiply     (×)
      0x81: U+2212 minus        (−)
      0x82: U+2026 ellipsis     (…)
      0x83: U+00B1 plus-minus   (±)
  - Tabular digits ('0'..'9' share a uniform advance width so timers never jitter)

Usage:
    python3 open_fw/tools/gen_aa_fonts.py --out open_fw/src/aa_fonts.c

SPDX-License-Identifier: MIT
"""

from __future__ import annotations

import argparse
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

DEFAULT_REGULAR = [
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
]

DEFAULT_BOLD = [
    "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
]

FIRST_CHAR = 0x20
EXTRA_CHARS = [
    (0x00B7, "MIDDLE DOT"),
    (0x00D7, "MULTIPLY"),
    (0x2212, "MINUS"),
    (0x2026, "ELLIPSIS"),
    (0x00B1, "PLUS-MINUS"),
]
LAST_CHAR = 0x7E + len(EXTRA_CHARS)


def pick_font(override: str | None, candidates: list[str]) -> str:
    if override:
        if not Path(override).is_file():
            raise FileNotFoundError(f"Font not found: {override}")
        return override
    for path in candidates:
        if Path(path).is_file():
            return path
    raise FileNotFoundError(f"No suitable TTF font found in: {candidates}")


def rasterize_font(ttf_path: str, px_size: int):
    font = ImageFont.truetype(ttf_path, px_size)
    ascent, descent = font.getmetrics()
    line_height = ascent + descent

    # Compute uniform tabular advance for digits '0'..'9'
    digit_adv = max(int(round(font.getlength(str(d)))) for d in range(10))

    glyphs = []
    blob = bytearray()

    chars_list = [(c, chr(c)) for c in range(FIRST_CHAR, 0x7F)]
    for idx, (cp, _) in enumerate(EXTRA_CHARS):
        chars_list.append((0x7F + idx, chr(cp)))

    for slot, ch in chars_list:
        raw_adv = max(1, int(round(font.getlength(ch))))
        is_digit = "0" <= ch <= "9"
        adv = digit_adv if is_digit else raw_adv

        if ch == " ":
            glyphs.append((0, 0, 0, 0, adv, 0))
            continue

        pad = 6
        w_canv = px_size * 2 + pad * 2
        h_canv = line_height + pad * 2
        img = Image.new("L", (w_canv, h_canv), 0)
        draw = ImageDraw.Draw(img)
        draw.text((pad, pad), ch, font=font, fill=255)
        bbox = img.getbbox()
        if bbox is None:
            glyphs.append((0, 0, 0, 0, adv, 0))
            continue

        x0, y0, x1, y1 = bbox
        gw = x1 - x0
        gh = y1 - y0
        x_off = x0 - pad
        if is_digit:
            # Center glyph cleanly inside uniform tabular cell
            x_off = (digit_adv - gw) // 2
        y_off = y0 - pad
        data_off = len(blob)

        px = img.load()
        nibbles = []
        for yy in range(y0, y1):
            for xx in range(x0, x1):
                a8 = px[xx, yy]
                a4 = (a8 * 15 + 127) // 255
                nibbles.append(a4)
        if len(nibbles) & 1:
            nibbles.append(0)
        for i in range(0, len(nibbles), 2):
            blob.append((nibbles[i] << 4) | nibbles[i + 1])

        glyphs.append((gw, gh, x_off, y_off, adv, data_off))

    return {
        "px_size": px_size,
        "ascent": ascent,
        "line_height": line_height,
        "digit_adv": digit_adv,
        "glyphs": glyphs,
        "blob": bytes(blob),
    }


def format_font_c(sym_name: str, desc: str, data: dict) -> str:
    glyphs = data["glyphs"]
    blob = data["blob"]
    lines = [
        f"/* {desc} ({len(glyphs)} glyphs, digit_adv={data['digit_adv']}px, {len(blob)} bytes packed 4-bit alpha) */",
        f"static const aa_glyph_t {sym_name}_glyphs[{len(glyphs)}] = {{",
    ]
    for idx, (gw, gh, xo, yo, adv, off) in enumerate(glyphs):
        slot = FIRST_CHAR + idx
        if slot <= 0x7E:
            ch_label = chr(slot).replace("\\", "\\\\").replace("*/", "* /")
            comment = f"0x{slot:02X} '{ch_label}'"
        else:
            cp, name = EXTRA_CHARS[slot - 0x7F]
            comment = f"0x{slot:02X} U+{cp:04X} {name}"
        lines.append(
            f"    {{ {gw:2d}u, {gh:2d}u, {xo:3d}, {yo:3d}, {adv:2d}u, {off:5d}u }}, /* {comment} */"
        )
    lines.append("};")
    lines.append("")
    lines.append(f"static const uint8_t {sym_name}_data[{len(blob)}] = {{")
    for i in range(0, len(blob), 16):
        chunk = ", ".join(f"0x{b:02X}u" for b in blob[i : i + 16])
        lines.append(f"    {chunk},")
    lines.append("};")
    lines.append("")
    lines.append(f"const aa_font_t {sym_name} = {{")
    lines.append(f"    {FIRST_CHAR}u,")
    lines.append(f"    {LAST_CHAR}u,")
    lines.append(f"    {data['line_height']}u,")
    lines.append(f"    {data['ascent']}u,")
    lines.append(f"    {sym_name}_glyphs,")
    lines.append(f"    {sym_name}_data,")
    lines.append("};")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate 4-bit AA C font tables for OpenFW")
    ap.add_argument("--regular", help="Path to regular sans-serif TTF")
    ap.add_argument("--bold", help="Path to bold sans-serif TTF")
    ap.add_argument("--out", required=True, help="Output C file path")
    args = ap.parse_args()

    reg_ttf = pick_font(args.regular, DEFAULT_REGULAR)
    bold_ttf = pick_font(args.bold, DEFAULT_BOLD)

    specs = [
        ("g_font_ui_sm", reg_ttf, 11, "UI Small 11px Regular"),
        ("g_font_ui_md", reg_ttf, 14, "UI Medium 14px Regular"),
        ("g_font_ui_lg", bold_ttf, 22, "UI Large 22px Bold"),
    ]

    sections = [
        "/*",
        " * Auto-generated 4-bit grayscale anti-aliased proportional fonts for Cassini OpenFW.",
        " * Generated by open_fw/tools/gen_aa_fonts.py — do not hand-edit.",
        " *",
        " * SPDX-License-Identifier: MIT",
        " */",
        '#include "openfw.h"',
        "",
    ]

    total_bytes = 0
    for sym, ttf, px, desc in specs:
        font_data = rasterize_font(ttf, px)
        sections.append(format_font_c(sym, desc, font_data))
        sections.append("")
        n_blob = len(font_data["blob"])
        n_tbl = len(font_data["glyphs"]) * 8
        total_bytes += n_blob + n_tbl
        print(
            f"{sym:14s} ({desc:22s}, digit={font_data['digit_adv']:2d}px): "
            f"{n_blob:5d} B bitmap + {n_tbl:4d} B table = {n_blob + n_tbl:5d} B"
        )

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(sections), encoding="utf-8")
    print(f"Wrote {out_path} (total font footprint: {total_bytes} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
