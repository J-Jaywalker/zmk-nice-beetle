#!/usr/bin/env python3
"""Check the central Beedly animation in a simulator frame dump (make sim-test).

    sim.exe --headless --script sim/examples/beedly.txt --frames DIR ...
    python sim/tools/check_beedly.py --frames DIR --script sim/examples/beedly.txt

The script carries the expectations as comment lines

    #@beedly <from_ms>..<to_ms> <tag>        tag = idle walk sprint shift ctrl alt capslock none

(virtual milliseconds, absolute: use `at` in the script). For every expectation the checker looks
at every frame the simulator dumped in that window, cuts the 32x32 Beedly slot out of the central
(left) screen and requires that

  * each slot image is pixel-identical to a frame of the expected Aseprite tag
    (sprite_assets/renders/beedly_left + tags.json, i.e. what `make beedly_left` compiled in):
    catches the wrong animation, a stale / overlapping second object, wrong rotation or mirroring,
    clipping and a wrong position; `none` = the slot is empty;
  * no frame of a Shift/Ctrl/Alt/CAPSLOCK window is identical to a frame of another tag (that
    would show another key's label, e.g. Alt flipping to "Ctrl");
  * every frame of the tag shows up when the window covers at least two cycles;
  * the mean time a frame stays up (whole cycles fully inside the window) is the configured
    per-frame time (read from the central build's autoconf.h), within --tolerance ms (single
    frames are quantised by LVGL's animation timer, ~40 ms steps);
  * the pixel rows just above and below the slot are empty across the whole screen width
    (Beedly does not touch the "BAT" text or the WPM graph).

Slot position: BEEDLY_SLOT_TOP / BEEDLY_SLOT_LEFT from widgets/beedly_slot.h. Screen layout:
sim/host/main.c (margin 4 px, gap 16 px, both times the scale).
Exit code 0 = all passed, 1 = a check failed, 2 = usage / input error.
"""
import argparse
import json
import re
import sys
from pathlib import Path

from PIL import Image

REPO = Path(__file__).resolve().parents[2]
SHIELD = REPO / "boards" / "shields" / "nice_oled" / "nice_epaper"
SLOT_H = SHIELD / "widgets" / "beedly_slot.h"
RENDERS = REPO / "sprite_assets" / "renders" / "beedly_left"
AUTOCONF = REPO / "sim" / "build" / "gen" / "central" / "include" / "generated" / "zephyr" / "autoconf.h"
SCREEN_W, SCREEN_H = 68, 160

sys.path.insert(0, str(REPO / "sprite_assets"))
from generate_beedly_left import TAGS, tag_frames  # noqa: E402

EXPECT_RE = re.compile(r"#@beedly\s+(\d+)\s*\.\.\s*(\d+)\s+(\w+)")
FRAME_RE = re.compile(r"frame_(\d+)ms\.png$")
WPM_TAGS = {"idle", "walk", "sprint"}  # may share frames (same beetle, no key label)


def fail(msg):
    print(f"check_beedly: {msg}", file=sys.stderr)
    sys.exit(2)


def slot_position(frame_w, frame_h):
    text = SLOT_H.read_text(encoding="utf-8")
    env = {"CANVAS_WIDTH": SCREEN_W, "CANVAS_HEIGHT": SCREEN_H,
           "BEEDLY_LUNA_FRAME_W": frame_w, "BEEDLY_LUNA_FRAME_H": frame_h}
    out = {}
    for name in ("BEEDLY_SLOT_TOP", "BEEDLY_SLOT_LEFT"):
        m = re.search(rf"#define\s+{name}\s+(.+)", text)
        if not m:
            fail(f"{name} not found in {SLOT_H}")
        expr = m.group(1).split("/*")[0].strip()
        for k, v in env.items():
            expr = expr.replace(k, str(v))
        if not re.fullmatch(r"[\d\s()+\-*/]+", expr):
            fail(f"cannot evaluate {name} = {m.group(1)}")
        out[name] = int(eval(expr.replace("/", "//")))  # noqa: S307 - digits and operators only
    return out["BEEDLY_SLOT_TOP"], out["BEEDLY_SLOT_LEFT"]


def frame_ms_config():
    """Per-frame ms for each tag, as the firmware computes them (beedly_slot.c)."""
    conf = {}
    if AUTOCONF.is_file():
        for m in re.finditer(r"#define (CONFIG_\w+) (\d+)", AUTOCONF.read_text(encoding="utf-8")):
            conf[m.group(1)] = int(m.group(2))
    else:
        print(f"note: {AUTOCONF} missing, using the Kconfig defaults")

    def get(name, default):
        return conf.get(name, default)

    mod = get("CONFIG_NICE_BEETLE_LUNA_MODIFIER_FRAME_MS", 600)
    caps = get("CONFIG_NICE_BEETLE_LUNA_CAPSLOCK_FRAME_MS", 300)
    return {
        "idle": get("CONFIG_NICE_BEETLE_LUNA_IDLE_FRAME_MS", 960),
        "walk": get("CONFIG_NICE_BEETLE_LUNA_WALK_FRAME_MS", 200),
        "sprint": get("CONFIG_NICE_BEETLE_LUNA_SPRINT_FRAME_MS", 200),
        "shift": mod, "ctrl": mod, "alt": mod, "capslock": caps,
    }


def load_tags():
    info = json.loads((RENDERS / "tags.json").read_text(encoding="utf-8"))
    by_lower = {k.lower(): v for k, v in info["tags"].items()}
    frames = {}
    for i in range(info["frame_count"]):
        with Image.open(RENDERS / f"frame_{i}.png") as img:
            up = img.convert("RGBA").rotate(90, expand=True)  # stored rotated clockwise
        w, h = up.size
        px = up.load()
        frames[i] = tuple(tuple(1 if px[x, y][3] >= 128 else 0 for x in range(w)) for y in range(h))
    tags = {c: tag_frames(by_lower[t.lower()]) for c, t, _ in TAGS}
    return frames, tags


def load_dump(folder: Path):
    shots = []
    for p in folder.iterdir():
        m = FRAME_RE.search(p.name)
        if m:
            shots.append((int(m.group(1)), p))
    if not shots:
        fail(f"no frame_<ms>ms.png in {folder}")
    return sorted(shots)


class Screen:
    """Pixel access to the central screen of one dumped frame (upright screen coordinates)."""

    def __init__(self, path):
        img = Image.open(path).convert("L")
        self.s = img.size[0] // 160  # width = scale * (2 * 4 + 16 + 2 * 68)
        if self.s < 1 or img.size != (160 * self.s, (SCREEN_H + 8) * self.s):
            fail(f"{path}: unexpected image size {img.size}")
        self.px = img.load()
        m = 4 * self.s
        self.m = m
        hist = {}
        for y in range(SCREEN_H):
            for x in range(SCREEN_W):
                v = self.raw(x, y)
                hist[v] = hist.get(v, 0) + 1
        self.bg = max(hist, key=hist.get)

    def raw(self, x, y):
        return self.px[self.m + x * self.s + self.s // 2, self.m + y * self.s + self.s // 2]

    def ink(self, x, y):
        return 1 if abs(self.raw(x, y) - self.bg) > 64 else 0

    def region(self, left, top, w, h):
        return tuple(tuple(self.ink(left + x, top + y) for x in range(w)) for y in range(h))

    def row_clear(self, y):
        return 0 <= y < SCREEN_H and not any(self.ink(x, y) for x in range(SCREEN_W))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--frames", type=Path, required=True, help="the sim.exe --frames folder")
    ap.add_argument("--script", type=Path, required=True, help="script with #@beedly lines")
    ap.add_argument("--tolerance", type=int, default=20, help="tolerance of the mean frame time in ms")
    a = ap.parse_args()

    frames, tags = load_tags()
    fw = len(frames[0][0])
    fh = len(frames[0])
    top, left = slot_position(fh, fw)  # header W/H are the stored (rotated) size
    frame_ms = frame_ms_config()
    lookup = {}
    for i, bits in frames.items():
        lookup.setdefault(bits, []).append(i)
    frame_tags = {i: [t for t, seq in tags.items() if i in seq] for i in frames}
    empty = tuple(tuple(0 for _ in range(fw)) for _ in range(fh))

    expectations = []
    for n, line in enumerate(a.script.read_text(encoding="utf-8").splitlines(), 1):
        m = EXPECT_RE.search(line)
        if m:
            t0, t1, tag = int(m.group(1)), int(m.group(2)), m.group(3).lower()
            if tag not in tags and tag != "none":
                fail(f"{a.script}:{n}: unknown tag '{tag}'")
            expectations.append((n, t0, t1, tag))
    if not expectations:
        fail(f"no #@beedly lines in {a.script}")

    shots = load_dump(a.frames)
    # slot timeline: (t, slot image, path), consecutive identical slot images merged
    timeline = []
    cache = {}
    end_time = shots[-1][0]
    for t, p in shots:
        scr = Screen(p)
        img = scr.region(left, top, fw, fh)
        clear = scr.row_clear(top - 1) and scr.row_clear(top + fh)
        cache[t] = clear
        if not timeline or timeline[-1][1] != img:
            timeline.append([t, img, p, clear])
        else:
            timeline[-1][3] = timeline[-1][3] and clear

    print(f"check_beedly: slot at top {top}, left {left} ({fw}x{fh}); {len(shots)} dumped frames, "
          f"{len(timeline)} slot changes; per-frame ms {frame_ms}")
    failures = 0
    for n, t0, t1, tag in expectations:
        segs = []
        for k, (t, img, p, clear) in enumerate(timeline):
            t_end = timeline[k + 1][0] if k + 1 < len(timeline) else max(end_time, t1) + 1
            if t_end > t0 and t <= t1:
                segs.append((t, t_end, img, p, clear))
        problems = []
        seen = []
        for t, t_end, img, p, clear in segs:
            if not clear:
                problems.append(f"{p.name}: ink in the row above/below the slot (overlap)")
            if tag == "none":
                if img != empty:
                    problems.append(f"{p.name}: slot not empty")
                continue
            idx = lookup.get(img)
            if idx is None:
                what = "empty slot" if img == empty else "image matching no Beedly frame"
                problems.append(f"{p.name} (t={t}): {what} (clipped, misplaced, overlapping or wrong art)")
                continue
            ok = [i for i in idx if tag in frame_tags[i]]
            if not ok:
                got = sorted({tt for i in idx for tt in frame_tags[i]})
                problems.append(f"{p.name} (t={t}): shows frame {idx} of tag(s) {got}, expected {tag}")
                continue
            others = sorted({tt for i in idx for tt in frame_tags[i]} - {tag})
            if others and not ({tag} | set(others)) <= WPM_TAGS:
                # e.g. an Alt frame that is pixel-identical to a Ctrl frame: the key label in the
                # art is wrong, the screen would flip between the two labels
                problems.append(f"{p.name} (t={t}): frame {idx} of {tag} is identical to a frame of "
                                f"{others} (copy-pasted art?)")
                continue
            seen.extend(ok)
        if tag != "none" and not problems:
            seq = tags[tag]
            cycle = frame_ms[tag] * len(seq)
            if t1 - t0 >= 2 * cycle and set(seq) - set(seen):
                problems.append(f"frames {sorted(set(seq) - set(seen))} of {tag} never shown "
                                f"(animation not running?)")
            inner = [(t_end - t) for t, t_end, *_ in segs[1:-1]]
            if len(set(seq)) > 1 and len(inner) >= len(seq):
                # LVGL advances animations from its timer (LV_DEF_REFR_PERIOD, run from ZMK's display
                # tick), so single frame times are quantised (a 100 ms frame shows for 40..160 ms);
                # the mean over whole cycles is what the configuration sets.
                k = len(inner) - len(inner) % len(seq)
                mean = sum(inner[:k]) / k
                if abs(mean - frame_ms[tag]) > a.tolerance:
                    problems.append(f"mean frame time {mean:.0f} ms (frames {inner}), expected "
                                    f"{frame_ms[tag]} +- {a.tolerance}")
        label = f"line {n}: {t0}..{t1} ms {tag}"
        if problems:
            failures += 1
            print(f"FAIL {label}")
            for pr in problems[:6]:
                print(f"     {pr}")
        else:
            inner = [(t_end - t) for t, t_end, *_ in segs[1:-1]]
            timing = (f", mean frame time {sum(inner) / len(inner):.0f} ms over {len(inner)}"
                      if inner else "")
            print(f"PASS {label} ({len(segs)} slot image(s){timing})")
    print(f"check_beedly: {len(expectations) - failures}/{len(expectations)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
