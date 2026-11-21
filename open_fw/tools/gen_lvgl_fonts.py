#!/usr/bin/env python3
"""
Generate 4-bit (16-level) anti-aliased LVGL v9 font tables (lv_font_t)
from system Noto Sans (or fallback) TTF fonts.

Features:
  - Printable ASCII (U+0020..U+007E)
  - UI typography & crisp symbol glyphs (sorted by Unicode codepoint):
      U+00B1 ±  plus-minus
      U+00B7 ·  middle dot
      U+00D7 ×  multiply
      U+2026 …  ellipsis
      U+2212 −  minus
      U+25A0 ■  stop / square
      U+25B2 ▲  up arrow
      U+25B6 ▶  play / right triangle
      U+25BC ▼  down arrow
      U+25C0 ◀  left triangle
      U+25CF ●  record dot / circle
      U+2713 ✓  checkmark
  - Uniform tabular advance for digits '0'..'9' so timers never jitter.
  - Zero Node.js dependency (uses Python + Pillow).

Usage:
    python3 open_fw/tools/gen_lvgl_fonts.py --out open_fw/src/ui/ui_fonts.c

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

DEFAULT_SYMBOL = [
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/noto/NotoSansSymbols2-Regular.ttf",
    "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
]

# Sorted ascending by Unicode codepoint (required for LVGL sparse cmap binary search)
EXTRA_CODEPOINTS = [
    0x00B1,  # ±
    0x00B7,  # ·
    0x00D7,  # ×
    0x2026,  # …
    0x2212,  # −
    0x25A0,  # ■
    0x25B2,  # ▲
    0x25B6,  # ▶
    0x25BC,  # ▼
    0x25C0,  # ◀
    0x25CF,  # ●
    0x2713,  # ✓
]


def pick_font(override: str | None, candidates: list[str]) -> str:
    if override:
        if not Path(override).is_file():
            raise FileNotFoundError(f"Font not found: {override}")
        return override
    for path in candidates:
        if Path(path).is_file():
            return path
    raise FileNotFoundError(f"No suitable TTF font found in: {candidates}")


def render_glyph(font: ImageFont.FreeTypeFont, sym_font: ImageFont.FreeTypeFont,
                 cp: int, px_size: int, ascent: int, line_height: int,
                 digit_adv: int):
    ch = chr(cp)
    use_font = sym_font if cp >= 0x2500 else font
    raw_adv = max(1, int(round(use_font.getlength(ch))))
    adv = digit_adv if ("0" <= ch <= "9") else raw_adv

    if ch == " ":
        return (0, 0, 0, 0, adv, b"")

    pad = 6
    w_canv = px_size * 2 + pad * 2
    h_canv = line_height + pad * 2
    img = Image.new("L", (w_canv, h_canv), 0)
    draw = ImageDraw.Draw(img)
    draw.text((pad, pad), ch, font=use_font, fill=255)
    bbox = img.getbbox()
    if bbox is None:
        return (0, 0, 0, 0, adv, b"")

    x0, y0, x1, y1 = bbox
    gw = x1 - x0
    gh = y1 - y0
    ofs_x = x0 - pad
    # LVGL ofs_y: distance from baseline to bottom of glyph box (upward positive)
    y_bottom = y1 - pad
    ofs_y = ascent - y_bottom

    # Pack 4-bit nibbles continuously (MSB nibble first, padded to byte at end of glyph)
    pixels = []
    for y in range(y0, y1):
        for x in range(x0, x1):
            v = img.getpixel((x, y))
            pixels.append((v * 15 + 127) // 255)

    packed = bytearray()
    for i in range(0, len(pixels), 2):
        hi = pixels[i]
        lo = pixels[i + 1] if (i + 1) < len(pixels) else 0
        packed.append(((hi & 0x0F) << 4) | (lo & 0x0F))

    return (gw, gh, ofs_x, ofs_y, adv, bytes(packed))


def build_font(name: str, ttf_path: str, sym_path: str, px_size: int) -> str:
    font = ImageFont.truetype(ttf_path, px_size)
    sym_font = ImageFont.truetype(sym_path, max(8, px_size - 1))
    ascent, descent = font.getmetrics()
    line_height = ascent + descent
    digit_adv = max(int(round(font.getlength(str(d)))) for d in range(10))

    # Glyph ID 0 is reserved by LVGL
    bitmap = bytearray()
    dscs = [(0, 0, 0, 0, 0, 0)]  # (bitmap_index, adv_w_fp4, box_w, box_h, ofs_x, ofs_y)

    # Range 1: ASCII 0x20..0x7E (95 glyphs, gid 1..95)
    for cp in range(0x20, 0x7F):
        gw, gh, ox, oy, adv, data = render_glyph(
            font, sym_font, cp, px_size, ascent, line_height, digit_adv
        )
        bidx = len(bitmap)
        bitmap.extend(data)
        dscs.append((bidx, adv * 16, gw, gh, ox, oy))

    # Range 2: Sparse extra Unicode codepoints (gid 96..)
    extra_gid_start = len(dscs)
    extra_min = EXTRA_CODEPOINTS[0]
    extra_max = EXTRA_CODEPOINTS[-1]
    unicode_list = [cp - extra_min for cp in EXTRA_CODEPOINTS]

    for cp in EXTRA_CODEPOINTS:
        gw, gh, ox, oy, adv, data = render_glyph(
            font, sym_font, cp, px_size, ascent, line_height, digit_adv
        )
        bidx = len(bitmap)
        bitmap.extend(data)
        dscs.append((bidx, adv * 16, gw, gh, ox, oy))

    lines: list[str] = []
    lines.append(f"/* {name}: {Path(ttf_path).name} {px_size}px, 4-bit AA, {len(bitmap)} B bitmap */")
    lines.append(f"static LV_ATTRIBUTE_LARGE_CONST const uint8_t {name}_bitmap[] = {{")
    for i in range(0, len(bitmap), 16):
        chunk = ", ".join(f"0x{b:02X}u" for b in bitmap[i:i + 16])
        lines.append(f"    {chunk},")
    lines.append("};")
    lines.append("")

    lines.append(f"static const lv_font_fmt_txt_glyph_dsc_t {name}_glyph_dsc[] = {{")
    for bidx, adv_fp4, gw, gh, ox, oy in dscs:
        lines.append(
            f"    {{ .bitmap_index = {bidx}u, .adv_w = {adv_fp4}u, "
            f".box_w = {gw}u, .box_h = {gh}u, .ofs_x = {ox}, .ofs_y = {oy} }},"
        )
    lines.append("};")
    lines.append("")

    ulist_str = ", ".join(f"0x{u:04X}u" for u in unicode_list)
    lines.append(f"static const uint16_t {name}_unicode_list[] = {{ {ulist_str} }};")
    lines.append("")

    lines.append(f"static const lv_font_fmt_txt_cmap_t {name}_cmaps[] = {{")
    lines.append("    {")
    lines.append("        .range_start = 0x20u,")
    lines.append("        .range_length = 95u,")
    lines.append("        .glyph_id_start = 1u,")
    lines.append("        .unicode_list = NULL,")
    lines.append("        .glyph_id_ofs_list = NULL,")
    lines.append("        .list_length = 0u,")
    lines.append("        .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY,")
    lines.append("    },")
    lines.append("    {")
    lines.append(f"        .range_start = 0x{extra_min:04X}u,")
    lines.append(f"        .range_length = {extra_max - extra_min + 1}u,")
    lines.append(f"        .glyph_id_start = {extra_gid_start}u,")
    lines.append(f"        .unicode_list = {name}_unicode_list,")
    lines.append("        .glyph_id_ofs_list = NULL,")
    lines.append(f"        .list_length = {len(unicode_list)}u,")
    lines.append("        .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY,")
    lines.append("    },")
    lines.append("};")
    lines.append("")

    lines.append(f"static const lv_font_fmt_txt_dsc_t {name}_dsc = {{")
    lines.append(f"    .glyph_bitmap = {name}_bitmap,")
    lines.append(f"    .glyph_dsc = {name}_glyph_dsc,")
    lines.append(f"    .cmaps = {name}_cmaps,")
    lines.append("    .kern_dsc = NULL,")
    lines.append("    .kern_scale = 0u,")
    lines.append("    .cmap_num = 2u,")
    lines.append("    .bpp = 4u,")
    lines.append("    .kern_classes = 0u,")
    lines.append("    .bitmap_format = 0u,")
    lines.append("};")
    lines.append("")

    lines.append(f"const lv_font_t {name} = {{")
    lines.append("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
    lines.append("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
    lines.append(f"    .line_height = {line_height},")
    lines.append(f"    .base_line = {descent},")
    lines.append("    .subpx = LV_FONT_SUBPX_NONE,")
    lines.append("    .underline_position = -1,")
    lines.append("    .underline_thickness = 1,")
    lines.append(f"    .dsc = &{name}_dsc,")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description="Generate LVGL v9 4-bit AA C font tables")
    ap.add_argument("--out", default="open_fw/src/ui/ui_fonts.c", help="Output C file path")
    ap.add_argument("--regular", default=None, help="Regular TTF font path")
    ap.add_argument("--bold", default=None, help="Bold TTF font path")
    ap.add_argument("--symbol", default=None, help="Symbol TTF font path")
    args = ap.parse_args()

    reg = pick_font(args.regular, DEFAULT_REGULAR)
    bold = pick_font(args.bold, DEFAULT_BOLD)
    sym = pick_font(args.symbol, DEFAULT_SYMBOL)

    parts = [
        "/*",
        " * Generated 4-bit (16-level) anti-aliased LVGL v9 Noto Sans fonts.",
        " * Do not edit manually; regenerate with: python3 open_fw/tools/gen_lvgl_fonts.py",
        " *",
        " * SPDX-License-Identifier: MIT",
        " */",
        '#include "lvgl.h"',
        "",
        build_font("ui_font_sm", reg, sym, 11),
        build_font("ui_font_md", reg, sym, 14),
        build_font("ui_font_lg", bold, sym, 22),
    ]

    out_path = Path(args.out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text("\n".join(parts), encoding="utf-8")
    print(f"Wrote {out_path} ({out_path.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
