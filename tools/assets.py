#!/usr/bin/env python3
"""
LVGL v8.3 Artwork Extractor & Patcher for Smartlink SL6801 (`pen133_2.0.65`).

Scans a local 2 MB SPI NOR firmware dump (`dumps/firmware_2M.bin`) for embedded
`lv_img_dsc_t` descriptors (RGB565 True Color, Chroma-Keyed, and 8-bit Indexed),
extracts every image to PNG (plus a labeled contact sheet), and can patch edited
PNGs back into a firmware image (in-place when the same dimensions/format, or
repointed into unused XIP flash at 0x00D7C000+).

SPDX-License-Identifier: MIT
"""
import argparse
import struct
import sys
from pathlib import Path
from typing import List, NamedTuple

from PIL import Image, ImageDraw

XIP_BASE = 0x00C00000
FIRM_START = 0x00E000
FIRM_END = 0x173E8C

CF_TRUE_COLOR = 4
CF_TRUE_COLOR_ALPHA = 5
CF_TRUE_COLOR_CHROMA_KEYED = 6
CF_INDEXED_8BIT = 10


class LvImgDesc(NamedTuple):
    desc_vma: int
    cf: int
    width: int
    height: int
    data_size: int
    data_vma: int


def scan_lvgl_images(data: bytes) -> List[LvImgDesc]:
    imgs: List[LvImgDesc] = []
    for off in range(FIRM_START, FIRM_END - 12, 4):
        hdr, dsize, ptr = struct.unpack_from("<III", data, off)
        cf = hdr & 0x1F
        az = (hdr >> 5) & 0x7
        res = (hdr >> 8) & 0x3
        w = (hdr >> 10) & 0x7FF
        h = (hdr >> 21) & 0x7FF
        if az != 0 or res != 0 or cf not in (CF_TRUE_COLOR, CF_TRUE_COLOR_ALPHA, CF_TRUE_COLOR_CHROMA_KEYED, CF_INDEXED_8BIT):
            continue
        if not (4 <= w <= 160 and 4 <= h <= 160):
            continue
        if not (XIP_BASE + FIRM_START <= ptr < XIP_BASE + FIRM_END):
            continue
        if cf in (CF_TRUE_COLOR, CF_TRUE_COLOR_CHROMA_KEYED):
            expected = w * h * 2
        elif cf == CF_TRUE_COLOR_ALPHA:
            expected = w * h * 3
        elif cf == CF_INDEXED_8BIT:
            expected = 256 * 4 + w * h
        else:
            continue
        if dsize != expected:
            continue
        if (ptr - XIP_BASE) + dsize > FIRM_END:
            continue
        imgs.append(LvImgDesc(XIP_BASE + off, cf, w, h, dsize, ptr))
    return imgs


def rgb565_to_rgb(val: int) -> tuple[int, int, int]:
    r = ((val >> 11) & 0x1F) * 255 // 31
    g = ((val >> 5) & 0x3F) * 255 // 63
    b = (val & 0x1F) * 255 // 31
    return (r, g, b)


def rgb_to_rgb565(r: int, g: int, b: int) -> int:
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def decode_lvgl_image(data: bytes, desc: LvImgDesc, swap16: bool = True) -> Image.Image:
    off = desc.data_vma - XIP_BASE
    raw = data[off : off + desc.data_size]
    w, h = desc.width, desc.height

    if desc.cf in (CF_TRUE_COLOR, CF_TRUE_COLOR_CHROMA_KEYED):
        img = Image.new("RGBA", (w, h))
        px = img.load()
        fmt = ">H" if swap16 else "<H"
        for y in range(h):
            for x in range(w):
                v = struct.unpack_from(fmt, raw, (y * w + x) * 2)[0]
                r, g, b = rgb565_to_rgb(v)
                a = 0 if (desc.cf == CF_TRUE_COLOR_CHROMA_KEYED and v == 0x07E0) else 255
                px[x, y] = (r, g, b, a)
        return img

    if desc.cf == CF_TRUE_COLOR_ALPHA:
        img = Image.new("RGBA", (w, h))
        px = img.load()
        fmt = ">H" if swap16 else "<H"
        for y in range(h):
            for x in range(w):
                pos = (y * w + x) * 3
                v = struct.unpack_from(fmt, raw, pos)[0]
                a = raw[pos + 2]
                r, g, b = rgb565_to_rgb(v)
                px[x, y] = (r, g, b, a)
        return img

    if desc.cf == CF_INDEXED_8BIT:
        # LVGL v8 indexed palette: 256 entries of lv_color32_t (BGRA little-endian)
        palette = []
        for i in range(256):
            b, g, r, a = raw[i * 4 : i * 4 + 4]
            palette.append((r, g, b, a if a != 0 else 255))
        img = Image.new("RGBA", (w, h))
        px = img.load()
        pix_base = 256 * 4
        for y in range(h):
            for x in range(w):
                idx = raw[pix_base + y * w + x]
                px[x, y] = palette[idx]
        return img

    raise ValueError(f"Unsupported color format cf={desc.cf}")


def encode_lvgl_image(png_path: Path, desc: LvImgDesc, swap16: bool = True) -> bytes:
    img = Image.open(png_path).convert("RGBA")
    if img.size != (desc.width, desc.height):
        img = img.resize((desc.width, desc.height), Image.Resampling.LANCZOS)
    w, h = desc.width, desc.height
    px = img.load()

    if desc.cf in (CF_TRUE_COLOR, CF_TRUE_COLOR_CHROMA_KEYED):
        out = bytearray(w * h * 2)
        fmt = ">H" if swap16 else "<H"
        for y in range(h):
            for x in range(w):
                r, g, b, a = px[x, y]
                if desc.cf == CF_TRUE_COLOR_CHROMA_KEYED and a < 128:
                    v = 0x07E0  # LV_COLOR_CHROMA_KEY (pure green)
                else:
                    v = rgb_to_rgb565(r, g, b)
                struct.pack_into(fmt, out, (y * w + x) * 2, v)
        return bytes(out)

    if desc.cf == CF_INDEXED_8BIT:
        q = img.convert("RGB").quantize(colors=256, method=Image.Quantize.MEDIANCUT)
        pal_rgb = q.getpalette() or [0] * 768
        out = bytearray(256 * 4 + w * h)
        for i in range(256):
            r = pal_rgb[i * 3] if i * 3 < len(pal_rgb) else 0
            g = pal_rgb[i * 3 + 1] if i * 3 + 1 < len(pal_rgb) else 0
            b = pal_rgb[i * 3 + 2] if i * 3 + 2 < len(pal_rgb) else 0
            out[i * 4 : i * 4 + 4] = bytes((b, g, r, 0xFF))
        q_px = q.load()
        base = 256 * 4
        for y in range(h):
            for x in range(w):
                out[base + y * w + x] = q_px[x, y]
        return bytes(out)

    raise ValueError(f"Encoding not yet supported for cf={desc.cf}")


def cmd_extract(args: argparse.Namespace) -> None:
    data = args.firmware.read_bytes()
    imgs = scan_lvgl_images(data)
    args.out.mkdir(parents=True, exist_ok=True)
    print(f"Found {len(imgs)} LVGL v8.3 image(s) in {args.firmware}:")

    cols = 5
    cell_w, cell_h = 110, 100
    rows = (len(imgs) + cols - 1) // cols
    sheet = Image.new("RGBA", (cols * cell_w, max(1, rows) * cell_h), (24, 24, 28, 255))
    draw = ImageDraw.Draw(sheet)

    for idx, desc in enumerate(imgs):
        im = decode_lvgl_image(data, desc, swap16=not args.no_swap16)
        fname = f"{idx:02d}_0x{desc.desc_vma:08X}_{desc.width}x{desc.height}_cf{desc.cf}.png"
        im.save(args.out / fname)
        print(f"  [{idx:02d}] desc=0x{desc.desc_vma:08X}  {desc.width:3d}x{desc.height:<3d}  cf={desc.cf:2d}  ({desc.data_size:5d} B) -> {fname}")

        cx = (idx % cols) * cell_w
        cy = (idx // cols) * cell_h
        # Draw dark checkerboard/frame behind icon
        draw.rectangle([cx + 4, cy + 4, cx + cell_w - 5, cy + cell_h - 5], outline=(70, 70, 80, 255))
        ox = cx + (cell_w - desc.width) // 2
        oy = cy + 20 + (cell_h - 24 - desc.height) // 2
        sheet.alpha_composite(im, (ox, oy))
        draw.text((cx + 6, cy + 6), f"#{idx:02d} {desc.desc_vma & 0xFFFFF:05X}", fill=(220, 220, 140, 255))

    sheet_path = args.out / "contact_sheet.png"
    sheet.save(sheet_path)
    print(f"\nSaved labeled contact sheet: {sheet_path}")


def cmd_replace(args: argparse.Namespace) -> None:
    data = bytearray(args.firmware.read_bytes())
    imgs = scan_lvgl_images(data)
    target_vma = int(args.desc_vma, 16)
    matches = [d for d in imgs if d.desc_vma == target_vma]
    if not matches:
        print(f"Error: no LVGL image descriptor found at 0x{target_vma:08X}", file=sys.stderr)
        sys.exit(1)
    desc = matches[0]
    encoded = encode_lvgl_image(args.png, desc, swap16=not args.no_swap16)
    assert len(encoded) == desc.data_size
    off = desc.data_vma - XIP_BASE
    data[off : off + desc.data_size] = encoded
    args.out.write_bytes(data)
    print(f"Replaced 0x{desc.desc_vma:08X} ({desc.width}x{desc.height} cf={desc.cf}, {desc.data_size} B) -> {args.out}")
    print("Note: run tools/patch_firmware.py on the output to recompute FIRM CRC-16 and build update.up!")


def main() -> None:
    p = argparse.ArgumentParser(description="Extract and replace LVGL v8.3 artwork in SL6801 firmware.")
    p.add_argument("--no-swap16", action="store_true", help="Decode/encode RGB565 little-endian without byte swap")
    sub = p.add_subparsers(dest="cmd", required=True)

    pe = sub.add_parser("extract", help="Extract all LVGL images to PNG + contact sheet")
    pe.add_argument("firmware", type=Path, help="Path to local 2 MB firmware dump")
    pe.add_argument("--out", type=Path, default=Path("dumps/art"), help="Output directory")
    pe.set_defaults(func=cmd_extract)

    pr = sub.add_parser("replace", help="Replace a single LVGL image in-place from a PNG")
    pr.add_argument("firmware", type=Path, help="Input 2 MB firmware binary")
    pr.add_argument("--desc-vma", required=True, help="Descriptor VMA hex (e.g. 0x00C94A08)")
    pr.add_argument("--png", type=Path, required=True, help="Replacement PNG image")
    pr.add_argument("--out", type=Path, required=True, help="Output 2 MB firmware binary")
    pr.set_defaults(func=cmd_replace)

    args = p.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
