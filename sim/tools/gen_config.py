#!/usr/bin/env python3
"""Generate everything the simulator build needs from the REAL config sources.

For each half (central = corne_left, peripheral = corne_right) this:

1. Builds a devicetree from: a small sim board stub (matrix transform copied
   from ZMK's corne.dtsi, a kscan mock, a stub SPI bus), this repo's
   nice_epaper.overlay, the studio snippet overlay flag (central only, as in
   alphred_kb's build.yaml) and the user's keymap.  It is preprocessed with gcc
   and run through Zephyr's own gen_defines.py / gen_driver_kconfig_dts.py, so
   the firmware sources see a genuine devicetree_generated.h.
2. Evaluates Kconfig with Zephyr's kconfiglib over ZMK's app/Kconfig, the
   nice_nano board, ZMK's corne/nice_view_adapter shields, this repo's
   Kconfig.shield / Kconfig.defconfig and the LVGL Kconfig, then merges the
   same .conf fragments a real build would (board defconfig, shield confs,
   nice_epaper.conf, snippet conf, user corne.conf) and writes autoconf.h.
3. Writes sim_keymap_info.h for the host UI (key labels, halves, layer names).
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SIM = os.path.dirname(HERE)
REPO = os.path.dirname(SIM)

ROLES = {
    "central": {"shields": ["corne_left", "nice_view_adapter", "nice_epaper"], "snippet": True},
    "peripheral": {"shields": ["corne_right", "nice_view_adapter", "nice_epaper"], "snippet": False},
}


def log(msg):
    print(f"[sim-gen] {msg}", flush=True)


def fwd(p):
    return os.path.abspath(p).replace("\\", "/")


def extract_node(text, label):
    """Return the text of a devicetree node `label: name { ... };` with balanced braces."""
    m = re.search(rf"\b{re.escape(label)}\s*:\s*[\w,@-]+\s*\{{", text)
    if not m:
        raise SystemExit(f"node {label} not found")
    i = m.end()
    depth = 1
    while depth:
        c = text[i]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        i += 1
    end = text.index(";", i) + 1
    return text[m.start():end]


def write(path, content):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    old = open(path, encoding="utf-8").read() if os.path.exists(path) else None
    if old != content:  # keep mtimes stable so make doesn't rebuild needlessly
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(content)


def gen_dts(role, cfg, paths, out, cc):
    zmk, zephyr = paths["zmk"], paths["zephyr"]
    corne_dtsi = open(os.path.join(zmk, "app/boards/shields/corne/corne.dtsi"), encoding="utf-8").read()
    transform = extract_node(corne_dtsi, "default_transform")
    side_overlay = os.path.join(zmk, f"app/boards/shields/corne/{cfg['shields'][0]}.overlay")
    side = open(side_overlay, encoding="utf-8").read()
    side = re.sub(r"#include\s+\"corne.dtsi\"", "", side)
    # the side overlays also configure kscan GPIOs we don't have; keep only transform tweaks
    side_tf = "\n".join(re.findall(r"&default_transform\s*\{[^}]*\};", side))
    dts = f"""/dts-v1/;
#include <dt-bindings/zmk/matrix_transform.h>

/* ---- sim board stub (replaces nice_nano_v2 + corne GPIO wiring) ---- */
/ {{
    #address-cells = <1>;
    #size-cells = <1>;
    model = "zmk-nice-beetle simulator ({role})";
    compatible = "zmk,sim";

    chosen {{
        zmk,kscan = &kscan0;
        zmk,matrix-transform = &default_transform;
        zmk,battery = &vbatt;
    }};

    /* nice_nano_v2.dts: battery gauge chosen node (a sim sensor driver binds to it) */
    vbatt: vbatt {{
        compatible = "zmk,battery-nrf-vddh";
    }};

    kscan0: kscan {{
        compatible = "zmk,kscan-mock";
        rows = <4>;
        columns = <12>;
        events = <0>;
    }};

    /* copied verbatim from ZMK app/boards/shields/corne/corne.dtsi */
    {transform}

    /* stands in for the nRF SPIM that nice_view_adapter maps to &nice_view_spi;
       an SPI-bus binding is needed so the overlay's sharp,ls0xx node binds and
       its width/height become devicetree macros like on the real build */
    nice_view_spi: spi@0 {{
        compatible = "zephyr,spi-emul-controller";
        reg = <0 4>;
        #address-cells = <1>;
        #size-cells = <0>;
        status = "disabled";
    }};
}};

/* {os.path.basename(side_overlay)} (matrix transform part only) */
{side_tf}

/* this repo's shield overlay, verbatim */
#include "{fwd(os.path.join(REPO, 'boards/shields/nice_oled/nice_epaper.overlay'))}"

/* the user's keymap, verbatim */
#include "{fwd(paths['keymap'])}"
"""
    d = os.path.join(out, role)
    write(os.path.join(d, "sim.dts"), dts)
    incs = [os.path.join(zmk, "app/dts"), os.path.join(zmk, "app/include"),
            os.path.join(zmk, "app/module/include"), os.path.join(zmk, "app/module/dts"),
            os.path.join(zephyr, "include"), os.path.join(zephyr, "dts/common"),
            os.path.join(zephyr, "dts"), os.path.dirname(paths["keymap"]),
            os.path.join(REPO, "boards/shields/nice_oled"),
            # this repo as a Zephyr module with `dts_root: .` (zephyr/module.yml): Zephyr adds
            # <root>/include and <root>/dts to the DTS include path (dt-bindings/zmk/beedly.h)
            os.path.join(REPO, "include"), os.path.join(REPO, "dts")]
    cmd = [cc, "-E", "-x", "assembler-with-cpp", "-nostdinc", "-undef", "-D__DTS__", "-P"]
    if cfg["snippet"]:
        cmd.append("-DZMK_BEHAVIORS_KEEP_ALL")  # studio-rpc-usb-uart snippet (build.yaml, left half)
    for i in incs:
        cmd += ["-I", fwd(i)]
    pre = os.path.join(d, "sim.dts.pre")
    cmd += [fwd(os.path.join(d, "sim.dts")), "-o", fwd(pre)]
    # gcc's helpers (cc1) need the toolchain's DLL dir on PATH, and a writable temp dir
    env = os.environ.copy()
    if os.path.dirname(cc):
        env["PATH"] = os.path.dirname(os.path.abspath(cc)) + os.pathsep + env.get("PATH", "")
    env["TMP"] = env["TEMP"] = env["TMPDIR"] = d
    r = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if r.returncode:
        raise SystemExit(f"DTS preprocessing failed:\n{r.stderr}")

    gdir = os.path.join(d, "include", "generated", "zephyr")  # Zephyr 4.x: <zephyr/devicetree_generated.h>
    os.makedirs(gdir, exist_ok=True)
    # ... and <dts_root>/dts/bindings to the binding directories (zmk,behavior-beedly)
    bindings = [os.path.join(zmk, "app/dts/bindings"), os.path.join(zmk, "app/module/dts/bindings"),
                os.path.join(zephyr, "dts/bindings"), os.path.join(REPO, "dts/bindings")]
    # Zephyr 4.1 splits devicetree parsing (gen_edt.py -> edt.pickle) from header
    # generation (gen_defines.py --edt-pickle)
    r = subprocess.run([sys.executable, os.path.join(zephyr, "scripts/dts/gen_edt.py"),
                        "--dts", pre, "--dtc-flags", "",
                        "--bindings-dirs", *bindings,
                        "--dts-out", os.path.join(d, "zephyr.dts"),
                        "--edt-pickle-out", os.path.join(d, "edt.pickle"),
                        "--vendor-prefixes", os.path.join(zephyr, "dts/bindings/vendor-prefixes.txt"),
                        "--vendor-prefixes", os.path.join(zmk, "app/dts/bindings/vendor-prefixes.txt")],
                       capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f"gen_edt failed:\n{r.stdout}\n{r.stderr}")
    if r.stderr.strip():
        log(f"gen_edt ({role}): {r.stderr.strip()}")
    r = subprocess.run([sys.executable, os.path.join(zephyr, "scripts/dts/gen_defines.py"),
                        "--edt-pickle", os.path.join(d, "edt.pickle"),
                        "--header-out", os.path.join(gdir, "devicetree_generated.h.new")],
                       capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f"gen_defines failed:\n{r.stdout}\n{r.stderr}")
    if r.stderr.strip():
        log(f"gen_defines ({role}): {r.stderr.strip()}")
    newh = os.path.join(gdir, "devicetree_generated.h.new")
    write(os.path.join(gdir, "devicetree_generated.h"), open(newh, encoding="utf-8").read())
    os.remove(newh)
    r = subprocess.run([sys.executable, os.path.join(zephyr, "scripts/dts/gen_driver_kconfig_dts.py"),
                        "--kconfig-out", os.path.join(d, "Kconfig.dts"), "--bindings-dirs", *bindings],
                       capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(f"gen_driver_kconfig_dts failed:\n{r.stderr}")


# Board: Zephyr 4.1 hardware model v2.  ZMK main has no `nice_nano_v2` any more; the
# equivalent board target is nice_nano@2.0.0//zmk (ZMK's 2025-12-09 Zephyr 4.1 blog), i.e.
# board nice_nano, revision 2.0.0, qualifiers /nrf52840/zmk.  The `zmk` variant lives in
# app/boards/nicekeyboards/nice_nano and extends the base board in
# app/module/boards/nicekeyboards/nice_nano (board.yml `extend:`).
LEGACY_BOARDS = {"nice_nano_v2": "nice_nano@2.0.0//zmk", "nice_nano": "nice_nano@1.0.0//zmk"}


def board_target(paths, quiet=False):
    """Board target from the keymap repo's build.yaml (as the GitHub build uses it)."""
    raw = "nice_nano//zmk"
    by = os.path.join(paths["cache"], "alphred_kb", "build.yaml")
    if os.path.exists(by):
        import yaml
        doc = yaml.safe_load(open(by, encoding="utf-8")) or {}
        boards = {e.get("board") for e in doc.get("include", []) if e.get("board")}
        boards |= set(doc.get("board", []) or [])
        if len(boards) == 1:
            raw = boards.pop()
    tgt = raw
    if raw in LEGACY_BOARDS:
        tgt = LEGACY_BOARDS[raw]
        if not quiet:
            log(f"WARNING: build.yaml uses the pre-Zephyr-4.1 board name '{raw}', which ZMK main no "
                f"longer knows (the real build would stop with an invalid-board error). The sim uses "
                f"the equivalent '{tgt}'; build.yaml should say 'nice_nano//zmk'.")
    m = re.fullmatch(r"([\w-]+)(?:@([\w.]+))?(/[\w/]*)?", tgt)
    if not m:
        raise SystemExit(f"cannot parse board target {raw!r}")
    name, rev, quals = m.group(1), m.group(2), m.group(3) or ""
    if name != "nice_nano":
        raise SystemExit(f"board {raw!r}: the simulator only models the nice!nano")
    rev = rev or "2.0.0"
    rev = {"1": "1.0.0", "2": "2.0.0"}.get(rev, rev)
    if quals.startswith("//"):
        quals = "/nrf52840/" + quals[2:]
    quals = quals.rstrip("/") or "/nrf52840"
    zmk = paths["zmk"]
    return {"name": name, "revision": rev, "qualifiers": quals, "raw": raw,
            "dirs": [os.path.join(zmk, "app/module/boards/nicekeyboards/nice_nano"),
                     os.path.join(zmk, "app/boards/nicekeyboards/nice_nano")]}


def board_strings(board):
    """zephyr_build_string(... MERGE REVERSE) and its SHORT variant (cmake/modules/extensions.cmake)."""
    segs = [x for x in board["qualifiers"].split("/") if x]
    rev = board["revision"].replace(".", "_")
    full = ["_".join([board["name"]] + segs), "_".join([board["name"]] + segs + [rev])]
    short = ["_".join([board["name"]] + segs[1:]), "_".join([board["name"]] + segs[1:] + [rev])]
    return full, short


def board_defconfigs(board):
    """zephyr_file(CONF_FILES ${BOARD_DIRECTORIES} DEFCONFIG ...)"""
    full, short = board_strings(board)
    found = []
    for d in board["dirs"]:
        for f, sh in zip(full, short):
            for n in (f, sh):
                p = os.path.join(d, f"{n}_defconfig")
                if os.path.exists(p) and p not in found:
                    found.append(p)
    return found


def gen_kconfig(role, cfg, paths, out):
    """Runs in a subprocess (kconfigfunctions reads EDT_PICKLE at import time)."""
    zmk, zephyr = paths["zmk"], paths["zephyr"]
    board = board_target(paths, quiet=True)
    d = os.path.join(out, role)
    write(os.path.join(d, "Kconfig.shield.defconfig"),
          f'osource "{fwd(REPO)}/boards/shields/*/Kconfig.defconfig"\n')
    write(os.path.join(d, "Kconfig.shield"),
          f'osource "{fwd(REPO)}/boards/shields/*/Kconfig.shield"\n')
    # Kconfig.modules as zephyr_module.py writes it: LVGL (its module.yml names
    # zephyr/Kconfig = `if LVGL rsource "../Kconfig" endif`) and ZMK's app/module.
    lv, zm = fwd(paths["lvgl"]), fwd(os.path.join(zmk, "app", "module"))
    write(os.path.join(d, "Kconfig.modules"),
          f'menu "lvgl ({lv})"\nif LVGL\nosource "{lv}/Kconfig"\nendif\n'
          f'config ZEPHYR_LVGL_MODULE\n\tbool\n\tdefault y\nendmenu\n'
          f'menu "zmk ({zm})"\nosource "{zm}/Kconfig"\n'
          f'config ZEPHYR_ZMK_MODULE\n\tbool\n\tdefault y\nendmenu\n')
    # $(KCONFIG_BINARY_DIR)/boards/Kconfig.* as boards.cmake writes them for HWMv2 (one
    # osource per board directory) + the symbols boards/Kconfig{,.v2} derive from $(BOARD).
    kbd = os.path.join(d, "boards")
    for kf in ("Kconfig.defconfig", f"Kconfig.{board['name']}", "Kconfig"):
        write(os.path.join(kbd, kf), "".join(f'osource "{fwd(x)}/{kf}"\n' for x in board["dirs"]))

    def up(x):
        return re.sub(r"[^A-Za-z0-9]", "_", x).upper()

    quals = board["qualifiers"]
    write(os.path.join(d, "Kconfig.board.v2"),
          f'config BOARD_{up(board["name"])}\n\tdef_bool y\n'
          f'config BOARD_{up(board["name"] + quals)}\n\tdef_bool y\n'
          f'config BOARD_QUALIFIERS\n\tstring\n\tdefault "{quals[1:]}"\n'
          f'config BOARD\n\tstring\n\tdefault "{board["name"]}"\n'
          f'config BOARD_REVISION\n\tdef_string "{board["revision"]}"\n'
          f'config BOARD_TARGET\n\tstring\n\tdefault "{board["name"]}@{board["revision"]}{quals}"\n')
    env = os.environ.copy()
    env.update({
        "srctree": fwd(os.path.join(SIM, "kconfig")),
        "ZEPHYR_BASE": fwd(zephyr),
        "KCONFIG_BINARY_DIR": fwd(d),
        "KCONFIG_BOARD_DIR": fwd(kbd),
        "BOARD_DIR": fwd(board["dirs"][0]),
        "ZMK_CONFIG": fwd(os.path.dirname(paths["keymap"])),
        "SHIELD_AS_LIST": ";".join(cfg["shields"]),
        "EDT_PICKLE": fwd(os.path.join(d, "edt.pickle")),
        "ARCH": "arm", "BOARD": board["name"], "BOARD_REVISION": board["revision"],
        "BOARD_QUALIFIERS": quals, "HWM_SCHEME": "v2",
        "KCONFIG_WARN_UNDEF": "n",
    })
    os.environ.update(env)
    sys.path.insert(0, os.path.join(zephyr, "scripts", "kconfig"))
    import kconfiglib  # noqa: E402

    kconf = kconfiglib.Kconfig(os.path.join(zmk, "app", "Kconfig"), warn=False, suppress_traceback=True)
    # Merge order of Zephyr 4.1 kconfig.cmake: BOARD_DEFCONFIG, CONF_FILE (ZMK's app/prj.conf),
    # shield_conf_files (each shield's <shield>.conf + boards/<board>.conf, then the ZMK config
    # dir's user .conf appended by ZMK's post_boards_shields.cmake), EXTRA_CONF_FILE (snippets).
    frags = board_defconfigs(board)
    if not frags:
        raise SystemExit(f"no board defconfig found for {board}")
    frags.append(os.path.join(zmk, "app/prj.conf"))
    full, short = board_strings(board)
    for s in cfg["shields"]:
        for base in SHIELD_DIRS(paths):
            if not os.path.exists(os.path.join(base, f"{s}.overlay")):
                continue
            p = os.path.join(base, f"{s}.conf")
            if os.path.exists(p):
                frags.append(p)
            for n in full + short:
                p = os.path.join(base, "boards", f"{n}.conf")
                if os.path.exists(p) and p not in frags:
                    frags.append(p)
    frags.append(paths["conf"])
    if cfg["snippet"]:
        frags.append(os.path.join(zmk, "app/snippets/studio-rpc-usb-uart/studio-rpc-usb-uart.conf"))
    frags.append(os.path.join(SIM, "kconfig", "sim.conf"))
    for i, f in enumerate(frags):
        kconf.load_config(f, replace=(i == 0))
    kconf.write_config(os.path.join(d, ".config"))
    kconf.write_autoconf(os.path.join(d, "include", "generated", "zephyr", "autoconf.h.new"))
    newh = os.path.join(d, "include", "generated", "zephyr", "autoconf.h.new")
    write(os.path.join(d, "include", "generated", "zephyr", "autoconf.h"), open(newh, encoding="utf-8").read())
    os.remove(newh)
    # Report config assignments that did not stick (typos, unmet deps) like Zephyr does
    msgs = [w for w in kconf.warnings if "was assigned the value" in w or "set more than once" in w]
    with open(os.path.join(d, "kconfig_warnings.txt"), "w") as f:
        f.write("fragments:\n" + "\n".join("  " + x for x in frags) + "\n\n" + "\n".join(kconf.warnings))
    for w in msgs:
        log(f"kconfig ({role}): {w.splitlines()[0]}")


def SHIELD_DIRS(paths):
    return [os.path.join(paths["zmk"], "app/boards/shields/corne"),
            os.path.join(paths["zmk"], "app/boards/shields/nice_view_adapter"),
            os.path.join(REPO, "boards/shields/nice_oled")]


def parse_bindings_text(keymap_text):
    """Light parser of the raw keymap to get human-readable labels for the UI only."""
    layers = []
    km = re.search(r"compatible\s*=\s*\"zmk,keymap\"", keymap_text)
    body = keymap_text[km.end():]
    for m in re.finditer(r"(\w[\w-]*)\s*\{(?P<inner>[^{}]*?)\};", body):
        inner = m.group("inner")
        b = re.search(r"\bbindings\s*=\s*<(.*?)>\s*;", inner, re.S)
        if not b:
            continue
        name = re.search(r"display-name\s*=\s*\"([^\"]*)\"", inner)
        toks = [t.strip() for t in re.split(r"(?=&)", re.sub(r"//.*", "", b.group(1))) if t.strip()]
        layers.append({"node": m.group(1), "name": name.group(1) if name else m.group(1), "bindings": toks})
    return layers


def short_label(binding):
    t = binding.split()
    beh, args = t[0], t[1:]
    if beh in ("&kp",) and args:
        return args[0]
    if beh in ("&mo", "&to", "&tog", "&sl") and args:
        return f"{beh[1:]} {args[0]}"
    if beh in ("&mt", "&lt", "&hm") and len(args) == 2:
        return f"{args[1]}/{args[0]}"
    if beh == "&bt" and args:
        return " ".join(args).replace("BT_", "")
    if beh == "&beedly" and args:
        return "pet " + args[0].replace("BEEDLY_", "").lower()
    if beh in ("&none",):
        return ""
    if beh in ("&trans",):
        return "▽"
    return (beh[1:] + " " + " ".join(args)).strip()


def c_str(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def gen_keymap_info(paths, out):
    import pickle
    sys.path.insert(0, os.path.join(paths["zephyr"], "scripts/dts/python-devicetree/src"))
    with open(os.path.join(out, "central", "edt.pickle"), "rb") as f:
        edt = pickle.load(f)
    tf = edt.chosen_node("zmk,matrix-transform")
    rc = tf.props["map"].val
    cols = tf.props["columns"].val
    positions = [{"row": (v >> 8) & 0xff, "col": v & 0xff} for v in rc]
    # In ZMK's corne_right.overlay, col-offset = 6: matrix columns >= 6 live on the right half.
    for p in positions:
        p["half"] = 1 if p["col"] >= cols // 2 else 0
    km = edt.compat2nodes["zmk,keymap"][0]
    layer_names = []
    for child in km.children.values():
        nm = child.props.get("display-name")
        layer_names.append(nm.val if nm else child.name)
    text = open(paths["keymap"], encoding="utf-8").read()
    layers = parse_bindings_text(text)
    nl = max(len(layers), 1)
    lines = ["/* generated by sim/tools/gen_config.py - do not edit */", "#pragma once",
             f"#define SIM_NUM_KEYS {len(positions)}", f"#define SIM_NUM_LAYERS {len(layers)}",
             "static const struct { unsigned char row, col, half; } sim_key_pos[] = {"]
    lines += [f"  {{{p['row']}, {p['col']}, {p['half']}}}," for p in positions]
    lines.append("};")
    lines.append(f"static const char *sim_layer_labels[{nl}][{len(positions)}] = {{")
    for l in layers:
        labs = [short_label(b) for b in l["bindings"]] + [""] * len(positions)
        lines.append("  {" + ", ".join(c_str(x) for x in labs[:len(positions)]) + "},")
    lines.append("};")
    lines.append(f"static const char *sim_layer_names[{nl}] = {{" +
                 ", ".join(c_str(l["name"]) for l in layers) + "};")
    write(os.path.join(out, "sim_keymap_info.h"), "\n".join(lines) + "\n")
    info = {"positions": positions, "layers": layers, "dt_layer_names": layer_names}
    write(os.path.join(out, "keymap_info.json"), json.dumps(info, indent=1))
    return info


def gen_board_info(paths, out):
    """Display geometry straight from the overlay's chosen zephyr,display node."""
    import pickle
    sys.path.insert(0, os.path.join(paths["zephyr"], "scripts/dts/python-devicetree/src"))
    with open(os.path.join(out, "central", "edt.pickle"), "rb") as f:
        edt = pickle.load(f)
    disp = edt.chosen_node("zephyr,display")
    raw = disp._node.props
    w = raw["width"].to_num()
    h = raw["height"].to_num()
    rot = raw["rotation"].to_num() if "rotation" in raw else 0
    compat = raw["compatible"].to_string() if "compatible" in raw else "?"
    spi_hz = raw["spi-max-frequency"].to_num() if "spi-max-frequency" in raw else 0
    extcomin = 1 if "extcomin-gpios" in raw else 0
    write(os.path.join(out, "sim_board_info.h"),
          "#pragma once\n"
          f"#define SIM_DISPLAY_WIDTH {w}\n#define SIM_DISPLAY_HEIGHT {h}\n"
          f"#define SIM_DISPLAY_ROTATION {rot}\n#define SIM_DISPLAY_COMPAT {c_str(compat)}\n"
          f"#define SIM_DISPLAY_SPI_HZ {spi_hz}\n#define SIM_DISPLAY_EXTCOMIN {extcomin}\n")
    return w, h, rot, compat


def _split_params(params):
    out, depth, cur = [], 0, ""
    for ch in params:
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
            continue
        depth += ch in "(["
        depth -= ch in ")]"
        cur += ch
    if cur.strip():
        out.append(cur)
    return [p.strip() for p in out]


def gen_syscalls(paths, out):
    """Zephyr generates <syscalls/*.h> with gen_syscalls.py; without userspace each
    wrapper is just `return z_impl_<name>(...)`.  We generate the same thing for
    every header that includes a syscalls/ header."""
    dirs = [os.path.join(paths["zmk"], "app/include"), os.path.join(paths["zephyr"], "include/zephyr")]
    count = 0
    for d in dirs:
        for root, _, files in os.walk(d):
            for fn in files:
                if not fn.endswith(".h"):
                    continue
                text = open(os.path.join(root, fn), encoding="utf-8", errors="replace").read()
                # Zephyr 4.x: <zephyr/syscalls/x.h>; ZMK still uses <syscalls/x.h>
                inc = re.search(r"#include\s*<((?:zephyr/)?syscalls/[\w.]+)>", text)
                if not inc:
                    continue
                body = ["/* generated by sim/tools/gen_config.py (no-userspace syscall wrappers) */",
                        "#pragma once"]
                clean = re.sub(r"/\*.*?\*/", lambda mm: "\n" * mm.group(0).count("\n"), text, flags=re.S)
                clean = re.sub(r"//[^\n]*", "", clean)
                # preprocessor condition in effect at each line, so each wrapper is
                # emitted under the same #if as its __syscall declaration
                conds, stack = [], []
                g = re.search(r"#\s*ifndef\s+(\w+)\s*\n\s*#\s*define\s+\1\b", clean)
                guard_cond = f"!defined({g.group(1)})" if g else None
                for line in clean.split("\n"):
                    ls = line.strip()
                    mm = re.match(r"#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)", ls)
                    if mm:
                        kw, expr = mm.group(1), mm.group(2).strip()
                        if kw == "ifdef":
                            stack.append([f"defined({expr})", []])
                        elif kw == "ifndef":
                            stack.append([f"!defined({expr})", []])
                        elif kw == "if":
                            stack.append([f"({expr})", []])
                        elif kw == "elif" and stack:
                            prev = stack[-1]
                            prev[1].append(prev[0])
                            prev[0] = f"({expr})"
                        elif kw == "else" and stack:
                            prev = stack[-1]
                            prev[1].append(prev[0])
                            prev[0] = "1"
                        elif kw == "endif" and stack:
                            stack.pop()
                    parts = []
                    for cur, negs in stack:
                        parts += [f"!{n}" for n in negs] + [cur]
                    parts = [x for x in parts if x != guard_cond]
                    conds.append(" && ".join(parts))
                line_of = []
                pos = 0
                for i, line in enumerate(clean.split("\n")):
                    line_of.append(pos)
                    pos += len(line) + 1
                import bisect
                for m in re.finditer(r"__syscall\s+([^;{(]*?)\b(\w+)\s*\(([^;{]*?)\)\s*;", clean, re.S):
                    ln = bisect.bisect_right(line_of, m.start()) - 1
                    cond = conds[ln] if ln < len(conds) else ""
                    ret, name, params = m.group(1).strip(), m.group(2), m.group(3).strip()
                    plist = [] if params in ("", "void") else _split_params(params)
                    names = []
                    for p in plist:
                        nm = re.findall(r"(\w+)\s*(?:\[[^\]]*\])?\s*$", p)
                        names.append(nm[0] if nm else "")
                    call = f"z_impl_{name}({', '.join(names)})"
                    stmt = f"{call};" if ret == "void" else f"return {call};"
                    fn = (f"extern {ret} z_impl_{name}({params or 'void'});\n"
                          f"static inline {ret} {name}({params or 'void'}) {{ {stmt} }}")
                    body.append(f"#if {cond}\n{fn}\n#endif" if cond else fn)
                write(os.path.join(out, *inc.group(1).split("/")), "\n".join(body) + "\n")
                count += 1
    return count


def behavior_sources(paths, out, role):
    """Which ZMK behavior drivers are enabled (DT_HAS_..._ENABLED) for a role."""
    ac = open(os.path.join(out, role, "include/generated/zephyr/autoconf.h"), encoding="utf-8").read()
    return sorted(set(re.findall(r"#define CONFIG_DT_HAS_ZMK_BEHAVIOR_(\w+)_ENABLED 1", ac)))


def gen_version(paths, out):
    """<zephyr/version.h> as Zephyr's cmake/gen_version_h.cmake writes it, from zephyr/VERSION."""
    v = dict(re.findall(r"^(\w+)\s*=\s*(\S*)", open(os.path.join(paths["zephyr"], "VERSION")).read(), re.M))
    ma, mi, pa = (int(v.get(k) or 0) for k in ("VERSION_MAJOR", "VERSION_MINOR", "PATCHLEVEL"))
    code = (ma << 16) | (mi << 8) | pa
    write(os.path.join(out, "zephyr", "version.h"),
          "/* generated by sim/tools/gen_config.py from zephyr/VERSION */\n#pragma once\n"
          f"#define ZEPHYR_VERSION_CODE {code}\n"
          "#define ZEPHYR_VERSION(a,b,c) (((a) << 16) + ((b) << 8) + (c))\n"
          f"#define KERNELVERSION 0x{code:x}00\n#define KERNEL_VERSION_NUMBER 0x{code:x}\n"
          f"#define KERNEL_VERSION_MAJOR {ma}\n#define KERNEL_VERSION_MINOR {mi}\n"
          f"#define KERNEL_PATCHLEVEL {pa}\n#define KERNEL_VERSION_STRING \"{ma}.{mi}.{pa}\"\n"
          f"#define KERNEL_VERSION_EXTENDED_STRING \"{ma}.{mi}.{pa}+0\"\n"
          f"#define KERNEL_VERSION_TWEAK_STRING \"{ma}.{mi}.{pa}+0\"\n"
          f"#define BUILD_VERSION v{ma}.{mi}.{pa}-sim\n")
    return f"{ma}.{mi}.{pa}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cache", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--keymap", default="")
    ap.add_argument("--conf", default="")
    ap.add_argument("--cc", default="gcc")
    ap.add_argument("--kconfig-role", default="")
    ap.add_argument("--force", action="store_true", help="regenerate even if no input changed")
    a = ap.parse_args()

    cache = os.path.abspath(a.cache)
    src = os.path.join(cache, "src")
    kb = os.path.join(cache, "alphred_kb", "config")
    paths = {"cache": cache, "zmk": os.path.join(src, "zmk"), "zephyr": os.path.join(src, "zephyr"),
             "lvgl": os.path.join(src, "lvgl"),
             "keymap": os.path.abspath(a.keymap or os.path.join(kb, "corne.keymap")),
             "conf": os.path.abspath(a.conf or os.path.join(kb, "corne.conf"))}
    out = os.path.abspath(a.out)

    if a.kconfig_role:
        gen_kconfig(a.kconfig_role, ROLES[a.kconfig_role], paths, out)
        return

    for p in ("keymap", "conf"):
        if not os.path.exists(paths[p]):
            raise SystemExit(f"{p} file not found: {paths[p]}")
    stamp = os.path.join(out, ".gen_stamp")
    sig = input_signature(paths, a)
    outputs = [os.path.join(out, r, "sources.mk") for r in ROLES] + [os.path.join(out, "sim_keymap_info.h")]
    if (not a.force and os.path.exists(stamp) and open(stamp).read() == sig
            and all(os.path.exists(o) for o in outputs)):
        log("config inputs unchanged, skipping generation (--force to redo)")
        return
    if os.path.exists(stamp):
        os.remove(stamp)
    log(f"keymap: {paths['keymap']}")
    log(f"conf:   {paths['conf']}")
    b = board_target(paths)
    log(f"board:  {b['raw']} -> {b['name']}@{b['revision']}{b['qualifiers']} (Zephyr HWMv2)")
    for role, cfg in ROLES.items():
        log(f"devicetree ({role}: {' '.join(cfg['shields'])})")
        gen_dts(role, cfg, paths, out, a.cc)
        log(f"kconfig ({role})")
        r = subprocess.run([sys.executable, __file__, "--cache", cache, "--out", out,
                            "--keymap", paths["keymap"], "--conf", paths["conf"],
                            "--kconfig-role", role])
        if r.returncode:
            raise SystemExit("kconfig evaluation failed")
    log(f"zephyr version: {gen_version(paths, out)}")
    n = gen_syscalls(paths, out)
    log(f"syscall wrappers: {n} headers")
    w, h, rot, compat = gen_board_info(paths, out)
    log(f"display from overlay: {compat} {w}x{h} rotation={rot}")
    info = gen_keymap_info(paths, out)
    log(f"keymap: {len(info['positions'])} keys, layers: {', '.join(l['name'] for l in info['layers'])}")
    for role in ROLES:
        bh = behavior_sources(paths, out, role)
        write(os.path.join(out, role, "behaviors.txt"), "\n".join(bh) + "\n")
        gen_sources(role, paths, out)
    with open(stamp, "w") as f:
        f.write(sig)
    log("done")


def input_signature(paths, a):
    """mtime/size of every file the generation reads (besides the pinned ZMK/Zephyr/LVGL
    checkouts, which deps.json identifies by sha), so `make sim-build` can skip
    regeneration when e.g. only a widget .c or an asset changed."""
    files = [paths["keymap"], paths["conf"], os.path.join(paths["cache"], "deps.json"),
             os.path.join(paths["cache"], "alphred_kb", "build.yaml")]
    for root in (os.path.join(SIM, "tools"), os.path.join(SIM, "kconfig")):
        for d, _, fs in os.walk(root):
            files += [os.path.join(d, f) for f in fs if not f.endswith(".pyc")]
    cfg_ext = (".overlay", ".conf", ".dtsi", ".dts", ".yml", ".yaml", ".keymap", ".txt")
    for d, _, fs in os.walk(os.path.join(REPO, "boards")):
        files += [os.path.join(d, f) for f in fs if f.startswith("Kconfig") or f.endswith(cfg_ext)]
    # module dts_root: bindings and the dt-bindings headers the keymap includes
    for sub in ("dts", "include"):
        for d, _, fs in os.walk(os.path.join(REPO, sub)):
            files += [os.path.join(d, f) for f in fs]
    files.append(os.path.join(REPO, "zephyr", "module.yml"))
    h = hashlib.sha1(repr((a.keymap, a.conf, a.cc, paths["keymap"], paths["conf"])).encode())
    for f in sorted(files):
        try:
            st = os.stat(f)
            h.update(f"{f}|{st.st_mtime_ns}|{st.st_size};".encode())
        except OSError:
            h.update(f"{f}|missing;".encode())
    return h.hexdigest()


# ZMK sources that talk to hardware/radio stacks and are replaced by sim/shim/src/*.
# Everything else in ZMK's CMake source list is compiled unmodified.
ZMK_REPLACED = {
    "src/physical_layouts.c": "kscan/physical-layout input is injected at its output (position events)",
    "src/ble.c": "BLE stack -> profile/host model in fake_hw.c",
    "src/hog.c": "HID-over-GATT -> host model in fake_hw.c",
    "src/usb_hid.c": "USB HID class -> host model in fake_hw.c",
    "src/split/bluetooth/central.c": "BLE split radio -> sim split transport in fake_hw.c",
    "src/split/bluetooth/peripheral.c": "BLE split radio -> sim split transport in fake_hw.c",
    "src/split/bluetooth/service.c": "BLE split radio -> sim split transport in fake_hw.c",
    "src/split/bluetooth/central_bas_proxy.c": "BLE battery service proxy (not modelled)",
}
ZMK_DROPPED_DIRS = ("src/studio/", "src/settings/", "src/pointing/")


def gen_sources(role, paths, out):
    sys.path.insert(0, HERE)
    import cmake_sources as cs
    cfg = cs.parse_autoconf(os.path.join(out, role, "include/generated/zephyr/autoconf.h"))
    zmk_app = os.path.join(paths["zmk"], "app")
    shield = cs.collect(os.path.join(REPO, "boards/shields/nice_oled/CMakeLists.txt"), cfg)
    zmk = cs.collect(os.path.join(zmk_app, "CMakeLists.txt"), cfg)
    lv = cs.collect(os.path.join(paths["zephyr"], "modules/lvgl/CMakeLists.txt"), cfg,
                    variables={"ZEPHYR_CURRENT_MODULE_DIR": paths["lvgl"],
                               "ZEPHYR_LVGL_MODULE_DIR": paths["lvgl"]})
    report = []
    fw = []  # (path, extra_flags)
    # Link order of the real build: libapp.a (shield target_sources(app) first, as the
    # shield CMake runs inside find_package(Zephyr), then ZMK's app sources), then the
    # zephyr libraries (shield zephyr_library_sources, LVGL module).
    for kind, p in shield:
        if kind == "app":
            fw.append((p, ""))
    for kind, p in zmk:
        rel = os.path.relpath(p, zmk_app).replace("\\", "/")
        if rel in ZMK_REPLACED:
            report.append(f"replaced  {rel}: {ZMK_REPLACED[rel]}")
            continue
        if rel.startswith(ZMK_DROPPED_DIRS):
            report.append(f"dropped   {rel}")
            continue
        fw.append((p, "-Dmain=zmk_sim_main" if rel == "src/main.c" else ""))
    for kind, p in shield:
        if kind != "app":
            fw.append((p, ""))
    lvgl_lib = []
    for kind, p in lv:
        if os.path.abspath(p).startswith(os.path.abspath(paths["lvgl"])):
            lvgl_lib.append(p)
        else:
            fw.append((p, ""))
    # LVGL 9.3's lv_iter.c calls lv_circle_buf_*, but Zephyr's CMake list omits
    # lv_circle_buf.c; the firmware links because --gc-sections drops the unused
    # lv_iter functions.  The DLL link has no section GC, so add the file (nothing
    # in the firmware calls it, so it changes no behaviour).
    for extra in ("src/misc/lv_circle_buf.c",):
        p = os.path.join(paths["lvgl"], *extra.split("/"))
        if lvgl_lib and os.path.exists(p) and p not in lvgl_lib:
            lvgl_lib.append(p)
    seen = set()
    fw = [x for x in fw if not (os.path.abspath(x[0]) in seen or seen.add(os.path.abspath(x[0])))]
    lvgl_lib = [x for x in lvgl_lib if not (os.path.abspath(x) in seen or seen.add(os.path.abspath(x)))]
    heap_flags = "-DCONFIG_SYS_HEAP_RUNTIME_STATS=1"
    # Zephyr 4.x moved sys_heap to lib/heap (heap_stats.c = CONFIG_SYS_HEAP_RUNTIME_STATS, which the
    # sim forces on to observe the pool; heap_info.c only so lvgl_mem.c links without --gc-sections)
    for f in ("heap.c", "heap_stats.c", "heap_info.c"):
        fw.append((os.path.join(paths["zephyr"], "lib/heap", f), heap_flags))
    shim = os.path.join(SIM, "shim", "src")
    for f in ("kernel.c", "sim_main.c", "fake_hw.c", "fake_display.c", "settings.c", "null_page.c"):
        fw.append((os.path.join(shim, f), ""))
    fw.append((os.path.join(shim, "lvgl_heap.c"), heap_flags))

    def rel(p):
        return os.path.relpath(p, REPO).replace("\\", "/")

    lines = [f"# generated by sim/tools/gen_config.py for role {role} - do not edit",
             f"SIM_{role.upper()}_OBJS :=", f"SIM_{role.upper()}_LVGL_OBJS :="]
    for i, (p, extra) in enumerate(fw):
        order = 1000 + i
        base = os.path.splitext(os.path.basename(p))[0]
        # the path hash keeps a stale .d of another source from ever matching this object
        tag = hashlib.sha1((rel(p) + extra).encode()).hexdigest()[:6]
        obj = f"$(SIM_BUILD)/{role}/fw/{order}_{base}_{tag}.o"
        lines.append(f"SIM_{role.upper()}_OBJS += {obj}")
        lines.append(f"{obj}: {rel(p)} | $(SIM_BUILD)/{role}/fw/.dir")
        lines.append(f"\t$(call sim_cc,{role},{order},{extra})")
    for p in lvgl_lib:
        r = rel(p)
        obj = f"$(SIM_BUILD)/{role}/lvgl/" + os.path.relpath(p, paths["lvgl"]).replace("\\", "/").replace("/", "__")[:-2] + ".o"
        lines.append(f"SIM_{role.upper()}_LVGL_OBJS += {obj}")
        lines.append(f"{obj}: {r} | $(SIM_BUILD)/{role}/lvgl/.dir")
        lines.append(f"\t$(call sim_cc,{role},9000,)")
    write(os.path.join(out, role, "sources.mk"), "\n".join(lines) + "\n")
    write(os.path.join(out, role, "sources.txt"),
          "\n".join(f"{1000 + i} {rel(p)} {e}" for i, (p, e) in enumerate(fw)) + "\n\n" +
          "\n".join(report) + "\n")
    log(f"sources ({role}): {len(fw)} firmware/glue files, {len(lvgl_lib)} LVGL files")


if __name__ == "__main__":
    main()
