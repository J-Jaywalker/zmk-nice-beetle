"""Central (left half) Beedly pipeline: beedly_left.aseprite -> PNG frames + tags -> C file (LVGL 9 I1).

    python sprite_assets/generate_beedly_left.py                # export with Aseprite, regenerate
    python sprite_assets/generate_beedly_left.py --skip-export  # no Aseprite: use the existing PNGs
    (make beedly_left / make beedly_left NO_ASEPRITE=1)

The sprite replaces the Luna dog on the central screen. Its animations are Aseprite TAGS, one per
behaviour; the frame indices are never hardcoded, they come from Aseprite's --list-tags data:

    Idle      WPM below the walk threshold (also shown while Gui/Super is held)
    Walk      WPM between the walk and sprint thresholds
    Sprint    WPM at or above the sprint threshold
    Shift     Shift held          Ctrl   Ctrl held          Alt   Alt held
    CAPSLOCK  Caps Lock on

Tag names are matched case-insensitively; a missing tag is an error. Other tags are ignored (with a
note). A tag can have any number of frames: the C file holds one array + count per tag, so adding
frames needs no firmware change. Tag direction forward / reverse / pingpong is honoured.

Steps:
 1. (unless --skip-export) Aseprite renders every frame (all visible layers) and writes the tag
    list. Each frame is normalised to the repo's PNG convention, opaque black ink on transparent,
    and rotated 90 degrees clockwise (the panel orientation, as for the peripheral Beedly and the
    Luna *_90 images), then saved to sprite_assets/renders/beedly_left/frame_<n>.png together with
    tags.json. Old files are replaced only after a successful export.
    Ink rule (--ink, default "light"): beedly_left.aseprite is drawn as a light beetle on a dark
    "bg" layer, so the LIGHT opaque pixels are the ink and the dark background is dropped. On the
    normal screen Beedly is drawn in black; with CONFIG_NICE_VIEW_WIDGET_INVERTED in white (palette
    PALETTE_INVERTED_WHITE_INK; the background is transparent in both). ("--ink dark" = the
    peripheral rule.)
 2. ALWAYS: renders/beedly_left/{frame_<n>.png,tags.json} -> nice_epaper/assets/beedly_luna_frames.c
    and beedly_luna_frames.h (deterministic: same inputs -> byte-identical files).
"""
import argparse
import json
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
import convert_to_lvgl as conv  # noqa: E402
from generate_lvgl_c_arrays import (  # noqa: E402
    ASE_DIR, RENDER_DIR, REPO, ascii_row, find_aseprite, frame_files, palette_define, rel,
    repair_windows_env,
)

NAME = "beedly_left"
ASE_FILE = ASE_DIR / f"{NAME}.aseprite"
OUT_DIR = RENDER_DIR / NAME
ASSETS = REPO / "boards" / "shields" / "nice_oled" / "nice_epaper" / "assets"
OUTPUT_C = ASSETS / "beedly_luna_frames.c"
OUTPUT_H = ASSETS / "beedly_luna_frames.h"
PREFIX = "beedly_luna"

# (C name, Aseprite tag name, what it is for) - the firmware uses exactly these.
TAGS = [
    ("idle", "Idle", "WPM below CONFIG_NICE_BEETLE_LUNA_WALK_WPM; also shown while Gui is held"),
    ("walk", "Walk", "WPM from CONFIG_NICE_BEETLE_LUNA_WALK_WPM up to the sprint threshold"),
    ("sprint", "Sprint", "WPM at or above CONFIG_NICE_BEETLE_LUNA_SPRINT_WPM"),
    ("shift", "Shift", "Shift held"),
    ("ctrl", "Ctrl", "Ctrl held"),
    ("alt", "Alt", "Alt held"),
    ("capslock", "CAPSLOCK", "Caps Lock on"),
]


# ---------------------------------------------------------------- export


def normalise(img: Image.Image, ink: str) -> Image.Image:
    """Return the frame as opaque black ink on transparent (the renders/ convention)."""
    rgba = img.convert("RGBA")
    w, h = rgba.size
    src = rgba.load()
    out = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    dst = out.load()
    for y in range(h):
        for x in range(w):
            r, g, b, a = src[x, y]
            lum = (r * 299 + g * 587 + b * 114) // 1000
            light = lum >= 128
            if a >= 128 and (light if ink == "light" else not light):
                dst[x, y] = (0, 0, 0, 255)
    return out


def parse_tags(data):
    """Aseprite --data json -> {name: {...}} and the frame count."""
    tags = {}
    for t in data.get("meta", {}).get("frameTags", []):
        tags[t["name"]] = {"from": int(t["from"]), "to": int(t["to"]),
                           "direction": t.get("direction", "forward")}
    frames = data.get("frames", [])
    durations = [int(f.get("duration", 0)) for f in (frames if isinstance(frames, list) else frames.values())]
    return tags, durations


def export(aseprite, ink, timeout):
    if not ASE_FILE.is_file():
        raise SystemExit(f"Missing {rel(ASE_FILE)}")
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    print(f"Exporting {rel(ASE_FILE)} -> {rel(OUT_DIR)}")
    with tempfile.TemporaryDirectory(prefix="beedly_left_export_") as tmp:
        tmp_dir = Path(tmp)
        data_file = tmp_dir / "data.json"
        cmd = [aseprite, "-b", "--list-tags", str(ASE_FILE),
               "--data", str(data_file), "--format", "json-array",
               "--save-as", str(tmp_dir / "frame_{frame}.png")]
        try:
            subprocess.run(cmd, check=True, stdin=subprocess.DEVNULL, capture_output=True,
                           text=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            raise SystemExit(
                f"Aseprite did not finish within {timeout} s (is a dialog open, e.g. first-run or\n"
                "update prompt? Start Aseprite once interactively). The PNGs were left untouched;\n"
                "use --skip-export / make beedly_left NO_ASEPRITE=1 to build from them."
            )
        except subprocess.CalledProcessError as e:
            raise SystemExit(f"Aseprite failed ({e.returncode}):\n{e.stdout}{e.stderr}")
        frames = frame_files(tmp_dir)
        if not frames:
            raise SystemExit(f"Aseprite produced no frame_<n>.png for {rel(ASE_FILE)}")
        tags, durations = parse_tags(json.loads(data_file.read_text(encoding="utf-8")))
        info = {"source": rel(ASE_FILE), "ink": ink, "frame_count": len(frames),
                "durations_ms": durations, "tags": tags}
        check_tags(info)  # fail before touching the existing renders
        for old in frame_files(OUT_DIR):
            old.unlink()
        for p in frames:
            with Image.open(p) as img:
                norm = normalise(img, ink)
            norm.rotate(-90, expand=True).save(OUT_DIR / p.name)  # 90 degrees clockwise
        (OUT_DIR / "tags.json").write_text(json.dumps(info, indent=2, sort_keys=True) + "\n",
                                           encoding="utf-8", newline="\n")
    print(f"  {len(frames)} frame(s), ink = {ink} pixels, rotated 90 degrees clockwise; "
          f"tags: " + ", ".join("%s %d-%d" % (n, t["from"], t["to"]) for n, t in tags.items()))


# ---------------------------------------------------------------- generate


def tag_frames(tag):
    seq = list(range(tag["from"], tag["to"] + 1))
    d = tag.get("direction", "forward")
    if d == "reverse":
        seq.reverse()
    elif d == "pingpong":
        seq = seq + seq[-2:0:-1]
    elif d == "pingpong_reverse":
        seq.reverse()
        seq = seq + seq[-2:0:-1]
    return seq


def check_tags(info):
    """Map our tag list onto the Aseprite tags; returns {c_name: [frame indices]}."""
    by_lower = {}
    for name, tag in info["tags"].items():
        by_lower.setdefault(name.lower(), []).append((name, tag))
    resolved, missing = {}, []
    for cname, tname, _ in TAGS:
        hits = by_lower.get(tname.lower(), [])
        if not hits:
            missing.append(tname)
            continue
        if len(hits) > 1:
            raise SystemExit(f"{info['source']}: tag '{tname}' exists {len(hits)} times "
                             f"({', '.join(h[0] for h in hits)}); tag names must be unique")
        seq = tag_frames(hits[0][1])
        bad = [i for i in seq if not 0 <= i < info["frame_count"]]
        if bad or not seq:
            raise SystemExit(f"{info['source']}: tag '{hits[0][0]}' refers to frames {bad or seq} "
                             f"but the sprite has {info['frame_count']} frame(s)")
        resolved[cname] = seq
    if missing:
        found = ", ".join(sorted(info["tags"])) or "(none)"
        raise SystemExit(
            f"{info['source']}: missing Aseprite tag(s): {', '.join(missing)}.\n"
            f"  Tags found: {found}\n"
            f"  The central Beedly needs one tag per animation: {', '.join(t for _, t, _ in TAGS)}\n"
            "  (names are case-insensitive; select the frames in Aseprite's timeline and use\n"
            "  Frame > Tags > New Tag)."
        )
    wanted = {t.lower() for _, t, _ in TAGS}
    extra = sorted(n for n in info["tags"] if n.lower() not in wanted)
    if extra:
        print(f"  note: ignoring tag(s) the firmware does not use: {', '.join(extra)}")
    return resolved


def load_renders():
    tags_file = OUT_DIR / "tags.json"
    if not tags_file.is_file():
        raise SystemExit(f"Missing {rel(tags_file)} (run `make beedly_left` with Aseprite once)")
    info = json.loads(tags_file.read_text(encoding="utf-8"))
    files = frame_files(OUT_DIR)
    if [p.name for p in files] != [f"frame_{i}.png" for i in range(len(files))]:
        raise SystemExit(f"{rel(OUT_DIR)}: frame_<n>.png files must be numbered 0..n-1 without gaps")
    if len(files) != info.get("frame_count"):
        print(f"  note: {len(files)} PNG(s) in {rel(OUT_DIR)} but tags.json says "
              f"{info.get('frame_count')}; using the PNGs")
        info["frame_count"] = len(files)
    resolved = check_tags(info)
    frames = {}
    for i in sorted({i for seq in resolved.values() for i in seq}):
        with Image.open(files[i]) as img:
            frames[i] = (files[i], *conv.image_to_bits(img))
    unused = sorted(set(range(len(files))) - set(frames))
    if unused:
        print(f"  note: frame(s) {unused} are in no tag and are not compiled in")
    return info, resolved, frames


def generate():
    info, resolved, frames = load_renders()
    # Frames shared between a key tag and another tag usually mean copy-pasted art (Alt frame
    # 11 once was a copy of a Ctrl frame, so holding Alt flipped between "ALT" and "Ctrl").
    wpm = {"idle", "walk", "sprint"}
    owners = {}
    for cname, seq in resolved.items():
        for i in seq:
            owners.setdefault(tuple(map(tuple, frames[i][3])), set()).add((cname, i))
    for same in owners.values():
        tags_ = {c for c, _ in same}
        if len(tags_) > 1 and not tags_ <= wpm:
            print("  WARNING: identical frames in different tags: " +
                  ", ".join(f"{c} frame {i}" for c, i in sorted(same, key=lambda x: x[1])) +
                  " (wrong key label copied?)")
    sizes = {(w, h) for _, w, h, _ in frames.values()}
    src = f"sprite_assets/ase/{NAME}.aseprite"
    c = [
        "/*",
        " * GENERATED FILE - do not edit by hand.",
        f" * Source: {src} (tags) -> sprite_assets/renders/{NAME}/frame_<n>.png + tags.json",
        " * Regenerate with `make beedly_left` (`make beedly_left NO_ASEPRITE=1` to skip the Aseprite",
        " * export). Used by widgets/beedly_slot.c (central screen, replaces the Luna dog).",
        " *",
        " * LVGL 9 LV_COLOR_FORMAT_I1 images: 2 palette entries (lv_color32_t: B, G, R, A), then rows",
        " * of (w + 7) / 8 bytes, MSB = leftmost pixel. Index 1 = Beedly ink, index 0 = background.",
        " * Frames are stored rotated 90 degrees clockwise (panel orientation).",
        " */",
        "",
        "#include <lvgl.h>",
        '#include "beedly_luna_frames.h"',
        "",
        "#ifndef LV_ATTRIBUTE_MEM_ALIGN",
        "#define LV_ATTRIBUTE_MEM_ALIGN",
        "#endif",
        "",
        "#ifdef CONFIG_NICE_VIEW_WIDGET_INVERTED",
        "/* index 0: transparent, index 1: opaque white (the inverted screen's foreground) */",
        palette_define(conv.PALETTE_INVERTED_WHITE_INK).replace("BEEDLY_PALETTE", "BEEDLY_LUNA_PALETTE"),
        "#else",
        "/* index 0: transparent, index 1: opaque black */",
        palette_define(conv.PALETTE_NORMAL).replace("BEEDLY_PALETTE", "BEEDLY_LUNA_PALETTE"),
        "#endif",
        "",
    ]
    for i, (path, w, h, rows) in frames.items():
        cname = f"{PREFIX}_{i}"
        stride = conv.stride_of(w)
        c.append(f"/* {NAME}/{path.name}: {w} x {h} px, stride {stride} */")
        c.append(f"static const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST uint8_t {cname}_map[] = {{")
        c.append("    BEEDLY_LUNA_PALETTE")
        for row, packed in zip(rows, conv.pack_rows(w, rows)):
            c.append(f"    {conv.fmt_bytes(packed)}, /* {ascii_row(row)} */")
        c += [
            "};",
            "",
            f"static const lv_image_dsc_t {cname} = {{",
            "    .header.magic = LV_IMAGE_HEADER_MAGIC,",
            "    .header.cf = LV_COLOR_FORMAT_I1,",
            "    .header.flags = 0,",
            f"    .header.w = {w},",
            f"    .header.h = {h},",
            f"    .header.stride = {stride},",
            f"    .data_size = sizeof({cname}_map),",
            f"    .data = {cname}_map,",
            "};",
            "",
        ]
    h = [
        "#pragma once",
        "",
        "/*",
        " * GENERATED FILE - do not edit by hand (`make beedly_left`, see beedly_luna_frames.c).",
        f" * Central-screen Beedly animations, one per Aseprite tag of {src}.",
        " * Each <tag>[] holds the tag's frames in play order, <tag>_count how many there are.",
        " */",
        "",
        "#include <stddef.h>",
        "#include <lvgl.h>",
        "",
    ]
    if len(sizes) == 1:
        (w, hh), = sizes
        h += [f"/* every frame is {w} x {hh} px (stored rotated, as drawn on the panel) */",
              f"#define BEEDLY_LUNA_FRAME_W {w}", f"#define BEEDLY_LUNA_FRAME_H {hh}", ""]
    else:
        print(f"  note: frames have different sizes {sorted(sizes)}; BEEDLY_LUNA_FRAME_W/H not defined")
    for cname, tname, what in TAGS:
        seq = resolved[cname]
        c.append(f"/* tag '{tname}': frames {seq} - {what} */")
        c.append(f"const lv_image_dsc_t *const {PREFIX}_{cname}[] = {{")
        c += [f"    &{PREFIX}_{i}," for i in seq]
        c.append("};")
        c.append(f"const size_t {PREFIX}_{cname}_count = sizeof({PREFIX}_{cname}) / sizeof({PREFIX}_{cname}[0]);")
        c.append("")
        h.append(f"/* {tname}: {what} */")
        h.append(f"extern const lv_image_dsc_t *const {PREFIX}_{cname}[];")
        h.append(f"extern const size_t {PREFIX}_{cname}_count;")
        h.append("")
    for path, lines in ((OUTPUT_H, h), (OUTPUT_C, c)):
        text = "\n".join(lines).rstrip("\n") + "\n"
        old = path.read_text(encoding="utf-8") if path.exists() else None
        if old == text:
            print(f"{rel(path)} is up to date")
        else:
            path.write_text(text, encoding="utf-8", newline="\n")
            print(f"Wrote {rel(path)}")
    print("  " + ", ".join(f"{c_}={len(resolved[c_])}" for c_, _, _ in TAGS) +
          f" ({len(frames)} distinct frame(s))")


def main():
    sys.stdout.reconfigure(encoding="utf-8")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--skip-export", action="store_true",
                    help="do not run Aseprite; regenerate the C file from the existing PNGs + tags.json")
    ap.add_argument("--aseprite", help="path to Aseprite (default: $ASEPRITE or C:\\Program Files\\Aseprite)")
    ap.add_argument("--ink", choices=("light", "dark"), default="light",
                    help="which opaque pixels of the sprite are Beedly (default: light)")
    ap.add_argument("--timeout", type=int, default=120, help="seconds to wait for the Aseprite export")
    args = ap.parse_args()
    if not args.skip_export:
        repair_windows_env()
        export(find_aseprite(args.aseprite), args.ink, args.timeout)
    generate()


if __name__ == "__main__":
    main()
