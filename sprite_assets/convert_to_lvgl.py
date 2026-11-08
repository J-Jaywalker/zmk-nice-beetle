"""Convert PNG images to LVGL v9 C arrays in LV_COLOR_FORMAT_I1 (indexed 1-bit).

Pixel rule (same as the sprites in sprite_assets/renders): a pixel is "ink" (palette index 1)
when it is opaque (alpha >= 128) and dark (luminance < 128); everything else (transparent or
light) is index 0. The firmware palette is
    index 0 = transparent, index 1 = opaque black                (normal)
    index 0 = opaque black, index 1 = transparent                (CONFIG_NICE_VIEW_WIDGET_INVERTED)
which is exactly what the nice!view widgets used under LVGL 8.

Rows are packed MSB first (leftmost pixel = bit 7), stride = (w + 7) // 8 bytes, padding bits 0.
LVGL 9 I1 data = 2 palette entries (lv_color32_t, byte order B, G, R, A) followed by the rows.

Library use: see generate_lvgl_c_arrays.py.
CLI (standalone, for images that are not Beedly frames):
    python sprite_assets/convert_to_lvgl.py <png file or directory> [...]
writes <name>.c next to each PNG with an `lv_image_dsc_t <name>` you can paste into an asset file.
"""
import os
import sys
from pathlib import Path

from PIL import Image

# lv_color32_t entries in memory order: blue, green, red, alpha.
PALETTE_NORMAL = ((0x00, 0x00, 0x00, 0x00), (0x00, 0x00, 0x00, 0xFF))
PALETTE_INVERTED = ((0x00, 0x00, 0x00, 0xFF), (0x00, 0x00, 0x00, 0x00))


def image_to_bits(img: Image.Image):
    """Return (w, h, rows) where rows[y][x] is 0/1 (1 = ink)."""
    rgba = img.convert("RGBA")
    w, h = rgba.size
    px = rgba.load()
    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            r, g, b, a = px[x, y]
            lum = (r * 299 + g * 587 + b * 114) // 1000
            row.append(1 if (a >= 128 and lum < 128) else 0)
        rows.append(row)
    return w, h, rows


def pack_rows(w, rows):
    """Pack 0/1 rows MSB-first into bytes; returns list of per-row byte lists."""
    stride = (w + 7) // 8
    packed = []
    for row in rows:
        out = [0] * stride
        for x, bit in enumerate(row):
            if bit:
                out[x >> 3] |= 0x80 >> (x & 7)
        packed.append(out)
    return packed


def stride_of(w):
    return (w + 7) // 8


def fmt_bytes(values):
    return ", ".join(f"0x{v:02x}" for v in values)


def palette_lines(palette, indent="    "):
    return [f"{indent}{fmt_bytes(c)}, /*Color of index {i}*/" for i, c in enumerate(palette)]


def standalone_c(name, w, h, rows):
    """A self-contained LVGL 9 C file (with INVERTED palette switch) for one image."""
    packed = pack_rows(w, rows)
    lines = [
        "#include <lvgl.h>",
        "",
        "#ifndef LV_ATTRIBUTE_MEM_ALIGN",
        "#define LV_ATTRIBUTE_MEM_ALIGN",
        "#endif",
        "",
        f"/* {w} x {h}, LV_COLOR_FORMAT_I1, stride {stride_of(w)} */",
        f"const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST uint8_t {name}_map[] = {{",
        "#ifdef CONFIG_NICE_VIEW_WIDGET_INVERTED",
        *palette_lines(PALETTE_INVERTED),
        "#else",
        *palette_lines(PALETTE_NORMAL),
        "#endif",
    ]
    for row in packed:
        lines.append(f"    {fmt_bytes(row)},")
    lines += [
        "};",
        "",
        f"const lv_image_dsc_t {name} = {{",
        "    .header.magic = LV_IMAGE_HEADER_MAGIC,",
        "    .header.cf = LV_COLOR_FORMAT_I1,",
        "    .header.flags = 0,",
        f"    .header.w = {w},",
        f"    .header.h = {h},",
        f"    .header.stride = {stride_of(w)},",
        f"    .data_size = sizeof({name}_map),",
        f"    .data = {name}_map,",
        "};",
        "",
    ]
    return "\n".join(lines)


def c_identifier(stem):
    s = "".join(ch if (ch.isalnum() or ch == "_") else "_" for ch in stem)
    return s if not s[0].isdigit() else "img_" + s


def convert_file(png_path: Path):
    w, h, rows = image_to_bits(Image.open(png_path))
    name = c_identifier(png_path.stem)
    out = png_path.with_suffix(".c")
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(standalone_c(name, w, h, rows))
    print(f"OK - {png_path} -> {out} ({w}x{h})")


def main(argv):
    targets = argv or ["."]
    for t in targets:
        p = Path(t)
        files = sorted(p.glob("*.png")) if p.is_dir() else [p]
        for f in files:
            convert_file(f)


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    main(sys.argv[1:])
