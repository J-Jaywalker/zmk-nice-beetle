#!/usr/bin/env python3
"""Edit -> see loop for the simulator (`make sim-watch`).

Starts the simulator window (sim.exe --watch) and polls the source tree.  When
something changes it rebuilds; sim.exe notices the rebuilt central.dll /
peripheral.dll and cold-boots that half from it, without closing the window.

* sprite sources (default sprite_assets/ase/**) changed -> `make <art target>`
  (default `beedly`, which writes the firmware asset C file) then the build
* firmware / shield / shim / config sources changed      -> the build only

The build is `make sim-build SIM_OFFLINE=1 ...`: no network, config generation
skipped when no config input changed, only changed objects recompile.  The
watcher exits when the simulator window is closed.
"""
import argparse
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
SKIP_DIRS = {".git", "__pycache__", "build", "renders"}


def snapshot(roots):
    snap = {}
    for root in roots:
        if os.path.isfile(root):
            try:
                st = os.stat(root)
                snap[root] = (st.st_mtime_ns, st.st_size)
            except OSError:
                pass
            continue
        for d, dirs, files in os.walk(root):
            dirs[:] = [x for x in dirs if x not in SKIP_DIRS]
            for f in files:
                if f.endswith((".pyc", ".o", ".d", ".tmp", "~")) or f.startswith(".#"):
                    continue
                p = os.path.join(d, f)
                try:
                    st = os.stat(p)
                except OSError:
                    continue
                snap[p] = (st.st_mtime_ns, st.st_size)
    return snap


def changed(a, b):
    return sorted(k for k in set(a) | set(b) if a.get(k) != b.get(k))


def rel(p):
    try:
        return os.path.relpath(p, REPO)
    except ValueError:
        return p


def run(cmd):
    print(f"[sim-watch] $ {' '.join(cmd)}", flush=True)
    t0 = time.time()
    r = subprocess.run(cmd, cwd=REPO)
    status = "ok" if r.returncode == 0 else f"FAILED ({r.returncode})"
    print(f"[sim-watch] {status} in {time.time() - t0:.1f} s", flush=True)
    return r.returncode == 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--make", default="make")
    ap.add_argument("--build-arg", action="append", default=[],
                    help="extra make arguments for the rebuild (VAR=value)")
    ap.add_argument("--art-target", default="beedly",
                    help="make target that turns the sprite sources into the asset C file ('' = none)")
    ap.add_argument("--art", action="append", default=None,
                    help="sprite source dirs/files (default sprite_assets/ase)")
    ap.add_argument("--src", action="append", default=None,
                    help="firmware source dirs/files (default boards, sim/shim, sim/kconfig)")
    ap.add_argument("--interval", type=float, default=0.4)
    ap.add_argument("sim", nargs=argparse.REMAINDER, help="-- sim.exe and its arguments")
    a = ap.parse_args()
    sim_cmd = a.sim[1:] if a.sim[:1] == ["--"] else a.sim
    if not sim_cmd:
        ap.error("missing -- sim.exe ...")

    art = [os.path.join(REPO, p) for p in (a.art or ["sprite_assets/ase"])]
    src = [os.path.join(REPO, p) for p in (a.src or ["boards", "sim/shim", "sim/kconfig"])]
    build = [a.make, "--no-print-directory", "sim-build", "SIM_OFFLINE=1", *a.build_arg]
    art_cmd = [a.make, "--no-print-directory", a.art_target] if a.art_target else None

    print(f"[sim-watch] starting {' '.join(sim_cmd)}", flush=True)
    sim = subprocess.Popen(sim_cmd, cwd=REPO)
    print("[sim-watch] watching " + ", ".join(rel(p) for p in art + src) +
          " - edit and save; close the simulator window to stop", flush=True)
    s_art, s_src = snapshot(art), snapshot(src)
    try:
        while sim.poll() is None:
            time.sleep(a.interval)
            n_art, n_src = snapshot(art), snapshot(src)
            c_art, c_src = changed(s_art, n_art), changed(s_src, n_src)
            if not (c_art or c_src):
                continue
            # let a burst of saves (or the exporter) finish
            while True:
                time.sleep(0.3)
                n2_art, n2_src = snapshot(art), snapshot(src)
                if n2_art == n_art and n2_src == n_src:
                    break
                n_art, n_src = n2_art, n2_src
            c_art, c_src = changed(s_art, n_art), changed(s_src, n_src)
            for p in (c_art + c_src)[:8]:
                print(f"[sim-watch] changed: {rel(p)}", flush=True)
            ok = True
            if c_art and art_cmd:
                ok = run(art_cmd)
            if ok:
                run(build)
            # the build/art step may itself have written watched files (the asset C file):
            # take the snapshot after it so they do not trigger a second rebuild
            s_art, s_src = snapshot(art), snapshot(src)
    except KeyboardInterrupt:
        sim.terminate()
    return sim.wait()


if __name__ == "__main__":
    sys.exit(main())
