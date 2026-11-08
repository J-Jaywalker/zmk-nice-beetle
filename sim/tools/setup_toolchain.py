#!/usr/bin/env python3
"""Check (and optionally install) the MSYS2 toolchain the simulator needs.

The simulator is built 32-bit (MSYS2 'mingw32' environment, i686 gcc) on
purpose: the nRF52840 is a 32-bit MCU, so LVGL objects have the same pointer
sizes / layouts and the real 4 KiB LVGL heap (CONFIG_LV_Z_MEM_POOL_SIZE)
behaves like on the keyboard.
"""
import argparse
import os
import shutil
import subprocess
import sys

PKGS = {
    "mingw32": ["mingw-w64-i686-gcc", "mingw-w64-i686-SDL2"],
    "ucrt64": ["mingw-w64-ucrt-x86_64-gcc", "mingw-w64-ucrt-x86_64-SDL2"],
    "mingw64": ["mingw-w64-x86_64-gcc", "mingw-w64-x86_64-SDL2"],
}
BASE_PKGS = ["make"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--msys2", default="C:/msys64")
    ap.add_argument("--env", default="mingw32")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--install", action="store_true")
    a = ap.parse_args()

    root = a.msys2
    if not os.path.isdir(root):
        # cygpath-style /c/msys64 given?
        if root.startswith("/") and len(root) > 2 and root[2] == "/":
            root = f"{root[1]}:/{root[3:]}"
    pacman = os.path.join(root, "usr", "bin", "pacman.exe")
    if not os.path.exists(pacman):
        print(f"[sim-deps] ERROR: MSYS2 not found at {root}.\n"
              "  Install MSYS2 from https://www.msys2.org/ (default location C:\\msys64),\n"
              "  or pass SIM_MSYS2=<path> to make.", file=sys.stderr)
        sys.exit(2)

    envbin = os.path.join(root, a.env, "bin")
    need = {
        "gcc": os.path.join(envbin, "gcc.exe"),
        "SDL2": os.path.join(envbin, "SDL2.dll"),
        "SDL2 headers": os.path.join(root, a.env, "include", "SDL2", "SDL.h"),
        "make": os.path.join(root, "usr", "bin", "make.exe"),
    }
    missing = [k for k, p in need.items() if not os.path.exists(p)]
    py_missing = []
    for mod in ("yaml",):
        try:
            __import__(mod)
        except ImportError:
            py_missing.append(mod)

    if a.install and missing:
        pk = PKGS.get(a.env, PKGS["mingw32"]) + BASE_PKGS
        print(f"[sim-deps] installing with pacman: {' '.join(pk)}")
        r = subprocess.run([pacman, "-S", "--needed", "--noconfirm", *pk])
        if r.returncode:
            sys.exit(r.returncode)
        missing = [k for k, p in need.items() if not os.path.exists(p)]
    if a.install and py_missing:
        print(f"[sim-deps] installing Python packages: pyyaml")
        subprocess.run([sys.executable, "-m", "pip", "install", "--user", "pyyaml"])
        py_missing = []

    if missing or py_missing:
        print("[sim-deps] ERROR: simulator toolchain incomplete:", file=sys.stderr)
        for k in missing:
            print(f"  - missing {k} ({need[k]})", file=sys.stderr)
        for m in py_missing:
            print(f"  - missing Python module '{m}' (pip install pyyaml)", file=sys.stderr)
        print("  Run `make sim-deps` to install them with MSYS2 pacman.", file=sys.stderr)
        sys.exit(2)
    if shutil.which("git") is None:
        print("[sim-deps] ERROR: git not found on PATH", file=sys.stderr)
        sys.exit(2)
    if a.install:
        print(f"[sim-deps] toolchain OK ({a.env}: {need['gcc']})")


if __name__ == "__main__":
    main()
