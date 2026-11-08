"""Beedly animation pipeline: Aseprite -> PNG frames -> firmware C file (LVGL 9, I1).

    python sprite_assets/generate_lvgl_c_arrays.py              # export beedly_ui.aseprite, regenerate
    python sprite_assets/generate_lvgl_c_arrays.py --dithered   # export beedly_ui_dithered.aseprite
    python sprite_assets/generate_lvgl_c_arrays.py --all        # export both
    python sprite_assets/generate_lvgl_c_arrays.py --skip-export   # no Aseprite: use existing PNGs

Steps:
 1. (unless --skip-export) Aseprite exports every frame of the selected .aseprite file(s) to
    sprite_assets/renders/<name>/frame_<n>.png, rotated 90 degrees clockwise (68x115 sprite ->
    115x68, the orientation the nice!view shows). The old frame_*.png there are replaced only
    after a successful export, so removed frames disappear and a failed export changes nothing.
 2. ALWAYS: both PNG folders (beedly_ui and beedly_ui_dithered) are converted into
    boards/shields/nice_oled/nice_epaper/assets/beedly_frames.c, which the firmware compiles
    directly. It holds both variants (selected by CONFIG_NICE_BEETLE_DITHER_BEEDLY), the palette
    switch for CONFIG_NICE_VIEW_WIDGET_INVERTED and the `beedly_frames[]` / `beedly_frame_count`
    table used by widgets/animation.c, so adding or removing frames needs no code change.

The output is deterministic (same PNGs -> byte-identical file), one pixel row per line.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import convert_to_lvgl as conv  # noqa: E402

REPO = Path(__file__).resolve().parent.parent
ASE_DIR = REPO / "sprite_assets" / "ase"
RENDER_DIR = REPO / "sprite_assets" / "renders"
OUTPUT = REPO / "boards" / "shields" / "nice_oled" / "nice_epaper" / "assets" / "beedly_frames.c"
DEFAULT_ASEPRITE = r"C:\Program Files\Aseprite\Aseprite.exe"

VARIANTS = {
    # key: (render/ase name, C name prefix)
    "plain": ("beedly_ui", "beedly_plain"),
    "dithered": ("beedly_ui_dithered", "beedly_dithered"),
}

FRAME_RE = re.compile(r"^frame_(\d+)\.png$", re.IGNORECASE)


def rel(p: Path):
    try:
        return p.relative_to(REPO).as_posix()
    except ValueError:
        return str(p)


def find_aseprite(explicit):
    for cand in (explicit, os.environ.get("ASEPRITE"), DEFAULT_ASEPRITE, shutil.which("aseprite")):
        if cand and Path(cand).is_file():
            return cand
    raise SystemExit(
        "Aseprite not found. Pass --aseprite PATH (or set ASEPRITE=...), or regenerate from the\n"
        "existing PNGs with --skip-export (make beedly NO_ASEPRITE=1)."
    )


def repair_windows_env():
    """MSYS2 make started from Git Bash hands its children an environment without APPDATA,
    LOCALAPPDATA, USERPROFILE, TEMP and TMP. Aseprite then never finishes (it cannot find its
    settings folder), so put them back from the Windows known-folder API before exporting."""
    if os.name != "nt":
        return
    import ctypes
    from ctypes import wintypes

    def folder(csidl):
        buf = ctypes.create_unicode_buffer(wintypes.MAX_PATH)
        if ctypes.windll.shell32.SHGetFolderPathW(None, csidl, None, 0, buf) == 0:
            return buf.value
        return None

    known = {"APPDATA": folder(0x001A), "LOCALAPPDATA": folder(0x001C), "USERPROFILE": folder(0x0028)}
    for key, val in known.items():
        if val and not os.environ.get(key):
            os.environ[key] = val
    if os.environ.get("LOCALAPPDATA"):
        tmp = os.path.join(os.environ["LOCALAPPDATA"], "Temp")
        for key in ("TEMP", "TMP"):
            if not os.environ.get(key) and os.path.isdir(tmp):
                os.environ[key] = tmp
    tempfile.tempdir = None  # re-evaluate TEMP/TMP


def frame_files(folder: Path):
    frames = []
    for p in folder.iterdir():
        m = FRAME_RE.match(p.name)
        if m:
            frames.append((int(m.group(1)), p))
    return [p for _, p in sorted(frames)]


def export_variant(aseprite, key, timeout):
    name, _ = VARIANTS[key]
    ase_file = ASE_DIR / f"{name}.aseprite"
    out_dir = RENDER_DIR / name
    out_dir.mkdir(parents=True, exist_ok=True)
    print(f"Exporting {rel(ase_file)} -> {rel(out_dir)}")
    # Export into a temporary folder first so a failed export leaves the existing PNGs alone.
    with tempfile.TemporaryDirectory(prefix="beedly_export_") as tmp:
        tmp_dir = Path(tmp)
        try:
            subprocess.run(
                [aseprite, "-b", str(ase_file), "--save-as", str(tmp_dir / "frame_{frame}.png")],
                check=True,
                stdin=subprocess.DEVNULL,
                capture_output=True,
                text=True,
                timeout=timeout,
            )
        except subprocess.TimeoutExpired:
            raise SystemExit(
                f"Aseprite did not finish within {timeout} s (is a dialog open, e.g. first-run or\n"
                "update prompt? Start Aseprite once interactively). The PNGs were left untouched;\n"
                "use --skip-export / make beedly NO_ASEPRITE=1 to build from them."
            )
        except subprocess.CalledProcessError as e:
            raise SystemExit(f"Aseprite failed ({e.returncode}):\n{e.stdout}{e.stderr}")
        frames = frame_files(tmp_dir)
        if not frames:
            raise SystemExit(f"Aseprite produced no frame_<n>.png for {rel(ase_file)}")
        for old in frame_files(out_dir):  # removed frames must disappear
            old.unlink()
        for p in frames:
            with Image.open(p) as img:
                rotated = img.rotate(-90, expand=True)  # 90 degrees clockwise
            rotated.save(out_dir / p.name)
    print(f"  {len(frames)} frame(s), rotated 90 degrees clockwise")


def load_variant(key):
    name, _ = VARIANTS[key]
    folder = RENDER_DIR / name
    if not folder.is_dir():
        raise SystemExit(f"Missing PNG folder {folder}")
    frames = frame_files(folder)
    if not frames:
        raise SystemExit(f"No frame_<n>.png files in {folder}")
    out = []
    for p in frames:
        with Image.open(p) as img:
            w, h, rows = conv.image_to_bits(img)
        out.append((p, w, h, rows))
    return out


def ascii_row(row):
    return "".join("#" if b else "." for b in row)


def emit_variant(lines, key, frames):
    name, prefix = VARIANTS[key]
    for idx, (path, w, h, rows) in enumerate(frames):
        cname = f"{prefix}_{idx}"
        stride = conv.stride_of(w)
        lines.append(f"/* {name}/{path.name}: {w} x {h} px, stride {stride} */")
        lines.append(f"static const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST uint8_t {cname}_map[] = {{")
        lines.append("    BEEDLY_PALETTE")
        for row, packed in zip(rows, conv.pack_rows(w, rows)):
            lines.append(f"    {conv.fmt_bytes(packed)}, /* {ascii_row(row)} */")
        lines.append("};")
        lines.append("")
        lines.append(f"static const lv_image_dsc_t {cname} = {{")
        lines.append("    .header.magic = LV_IMAGE_HEADER_MAGIC,")
        lines.append("    .header.cf = LV_COLOR_FORMAT_I1,")
        lines.append("    .header.flags = 0,")
        lines.append(f"    .header.w = {w},")
        lines.append(f"    .header.h = {h},")
        lines.append(f"    .header.stride = {stride},")
        lines.append(f"    .data_size = sizeof({cname}_map),")
        lines.append(f"    .data = {cname}_map,")
        lines.append("};")
        lines.append("")
    lines.append("const lv_image_dsc_t *const beedly_frames[] = {")
    for idx in range(len(frames)):
        lines.append(f"    &{prefix}_{idx},")
    lines.append("};")


def palette_define(palette):
    return "#define BEEDLY_PALETTE " + ", ".join(conv.fmt_bytes(c) for c in palette) + ","


def generate(output: Path):
    plain = load_variant("plain")
    dithered = load_variant("dithered")
    lines = [
        "/*",
        " * GENERATED FILE - do not edit by hand.",
        " * Source: sprite_assets/ase/beedly_ui{,_dithered}.aseprite",
        " *      -> sprite_assets/renders/beedly_ui{,_dithered}/frame_<n>.png",
        " * Regenerate with `make beedly` (`make beedly NO_ASEPRITE=1` to skip the Aseprite export).",
        " *",
        " * LVGL 9 LV_COLOR_FORMAT_I1 images: 2 palette entries (lv_color32_t: B, G, R, A), then rows",
        " * of (w + 7) / 8 bytes, MSB = leftmost pixel. Index 1 = Beedly ink, index 0 = background.",
        " */",
        "",
        "#include <lvgl.h>",
        '#include "beedly_frames.h"',
        "",
        "#ifndef LV_ATTRIBUTE_MEM_ALIGN",
        "#define LV_ATTRIBUTE_MEM_ALIGN",
        "#endif",
        "",
        "#ifdef CONFIG_NICE_VIEW_WIDGET_INVERTED",
        "/* index 0: opaque black, index 1: transparent */",
        palette_define(conv.PALETTE_INVERTED),
        "#else",
        "/* index 0: transparent, index 1: opaque black */",
        palette_define(conv.PALETTE_NORMAL),
        "#endif",
        "",
        "#ifdef CONFIG_NICE_BEETLE_DITHER_BEEDLY",
        "",
    ]
    emit_variant(lines, "dithered", dithered)
    lines += ["", "#else /* !CONFIG_NICE_BEETLE_DITHER_BEEDLY */", ""]
    emit_variant(lines, "plain", plain)
    lines += [
        "",
        "#endif /* CONFIG_NICE_BEETLE_DITHER_BEEDLY */",
        "",
        "const size_t beedly_frame_count = sizeof(beedly_frames) / sizeof(beedly_frames[0]);",
        "",
    ]
    text = "\n".join(lines)
    old = output.read_text(encoding="utf-8") if output.exists() else None
    if old == text:
        print(f"{rel(output)} is up to date ({len(plain)} plain, {len(dithered)} dithered frames)")
        return
    output.write_text(text, encoding="utf-8", newline="\n")
    print(f"Wrote {rel(output)} ({len(plain)} plain, {len(dithered)} dithered frames)")


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--dithered", action="store_true", help="export the dithered sprite (instead of the plain one)")
    g.add_argument("--all", action="store_true", help="export both sprites")
    ap.add_argument("--skip-export", action="store_true",
                    help="do not run Aseprite; regenerate the C file from the existing PNGs")
    ap.add_argument("--aseprite", help=f"path to Aseprite (default: $ASEPRITE or {DEFAULT_ASEPRITE})")
    ap.add_argument("--timeout", type=int, default=120, help="seconds to wait for each Aseprite export")
    ap.add_argument("--output", type=Path, default=OUTPUT, help="C file to write (default: %(default)s)")
    args = ap.parse_args()

    if not args.skip_export:
        repair_windows_env()
        aseprite = find_aseprite(args.aseprite)
        keys = ["plain", "dithered"] if args.all else ["dithered"] if args.dithered else ["plain"]
        for key in keys:
            export_variant(aseprite, key, args.timeout)

    generate(args.output.resolve())


if __name__ == "__main__":
    main()
