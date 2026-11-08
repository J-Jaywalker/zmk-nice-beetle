#!/usr/bin/env python3
"""Fetch everything the simulator needs into the git-ignored cache.

* the keymap repo (default: J-Jaywalker/alphred_kb) unless KEYMAP points elsewhere
* ZMK at the revision named in the keymap repo's config/west.yml
* the Zephyr fork + LVGL revisions that ZMK revision pins (via west.yml files)

All clones are shallow, sparse where possible, and are only re-fetched when the
resolved commit changes.  Writes <cache>/deps.json describing what was fetched.

The network is only touched when needed: if the previous fetch was for the
same inputs and is younger than --max-age seconds (default 1 h), nothing is
re-resolved (pass --refresh, i.e. `make sim SIM_REFRESH=1`, to force a check
for a newer ZMK `main`; `--offline` never touches the network).

ZMK_REV=<sha|branch|tag> overrides the revision west.yml names.  If the
resolved ZMK ships a different LVGL major than the widget sources are written
for (--widget-lvgl, default 9) a warning is printed: the real firmware build
would not compile either.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import urllib.request

import yaml

import time

# ZMK main (Zephyr 4.1, HWMv2): boards live in app/boards/<vendor>/<board> (the
# nice_nano//zmk variant) extending app/module/boards/<vendor>/<board>; drivers
# and their DT bindings are in the app/module Zephyr module.
ZMK_SPARSE = [
    "app/include", "app/dts", "app/src", "app/boards", "app/snippets",
    "app/module", "app/keymap-module",
]
ZEPHYR_SPARSE = ["include", "scripts/dts", "scripts/kconfig", "dts/bindings", "dts/common",
                 "modules/lvgl", "lib/os", "lib/heap", "drivers/display"]


def log(msg):
    print(f"[sim-fetch] {msg}", flush=True)


def warn(msg):
    bar = "!" * 78
    print(f"\n{bar}\n[sim-fetch] WARNING: {msg}\n{bar}\n", flush=True)


def run(cmd, cwd=None, check=True, capture=False):
    res = subprocess.run(cmd, cwd=cwd, check=False, text=True,
                         stdout=subprocess.PIPE if capture else None,
                         stderr=subprocess.PIPE if capture else None)
    if check and res.returncode != 0:
        raise SystemExit(f"command failed ({res.returncode}): {' '.join(cmd)}\n"
                         f"{res.stderr if capture else ''}")
    return res


def http_get(url):
    try:
        with urllib.request.urlopen(url, timeout=30) as r:
            return r.read().decode("utf-8")
    except Exception as e:  # noqa: BLE001
        return None


def ls_remote(url, ref):
    """Resolve a branch/tag name to a sha. Returns ref itself if it already looks like a sha."""
    if re.fullmatch(r"[0-9a-f]{40}", ref):
        return ref
    res = run(["git", "ls-remote", url, ref, f"refs/heads/{ref}", f"refs/tags/{ref}",
               f"refs/tags/{ref}^{{}}"], check=False, capture=True)
    if res.returncode != 0 or not res.stdout.strip():
        return None
    lines = [l.split() for l in res.stdout.strip().splitlines()]
    # prefer peeled tag, then branch, then anything
    for sha, name in lines:
        if name.endswith("^{}"):
            return sha
    for sha, name in lines:
        if name == f"refs/heads/{ref}":
            return sha
    return lines[0][0]


def stamp_ok(path, sha):
    try:
        return run(["git", "rev-parse", "HEAD"], cwd=path, capture=True, check=False).stdout.strip() == sha
    except Exception:  # noqa: BLE001
        return False


def shallow_fetch(path, url, sha, sparse=None):
    if os.path.isdir(os.path.join(path, ".git")) and stamp_ok(path, sha):
        if sparse:
            run(["git", "sparse-checkout", "set", *sparse], cwd=path)
        log(f"{os.path.basename(path)} already at {sha[:10]}")
        return
    os.makedirs(path, exist_ok=True)
    if not os.path.isdir(os.path.join(path, ".git")):
        run(["git", "init", "-q"], cwd=path)
        run(["git", "remote", "add", "origin", url], cwd=path)
    else:
        run(["git", "remote", "set-url", "origin", url], cwd=path)
    if sparse:
        run(["git", "sparse-checkout", "set", *sparse], cwd=path)
    log(f"fetching {url} @ {sha[:10]} -> {path}")
    run(["git", "fetch", "-q", "--depth", "1", "origin", sha], cwd=path)
    run(["git", "checkout", "-q", "--force", "FETCH_HEAD"], cwd=path)


def west_projects(text):
    m = yaml.safe_load(text)["manifest"]
    remotes = {r["name"]: r["url-base"] for r in m.get("remotes", [])}
    default_remote = (m.get("defaults") or {}).get("remote")
    out = {}
    for p in m.get("projects", []):
        remote = p.get("remote", default_remote)
        base = remotes.get(remote, f"https://github.com/{remote}" if remote else None)
        repo = p.get("repo-path", p["name"])
        out[p["name"]] = {"url": f"{base}/{repo}" if base else None,
                          "revision": str(p.get("revision", "main"))}
    return out


def raw_url(git_url, sha, path):
    m = re.match(r"https://github.com/([^/]+)/([^/.]+)", git_url)
    return f"https://raw.githubusercontent.com/{m.group(1)}/{m.group(2)}/{sha}/{path}"


def lvgl_for_zmk(zmk_url, zmk_sha):
    """Return (zephyr_url, zephyr_sha, lvgl_url, lvgl_sha, lvgl_major) for a zmk commit."""
    zw = http_get(raw_url(zmk_url, zmk_sha, "app/west.yml"))
    if zw is None:
        raise SystemExit("could not download ZMK app/west.yml (offline?)")
    zp = west_projects(zw)
    zephyr = zp["zephyr"]
    zephyr_sha = ls_remote(zephyr["url"], zephyr["revision"])
    if zmk_lvgl := zp.get("lvgl"):
        lvgl_url = zmk_lvgl["url"]
        lvgl_sha = ls_remote(lvgl_url, zmk_lvgl["revision"])
    else:
        yw = http_get(raw_url(zephyr["url"], zephyr_sha, "west.yml"))
        yp = west_projects(yw)
        lvgl_url = yp["lvgl"]["url"] or "https://github.com/zephyrproject-rtos/lvgl"
        lvgl_sha = ls_remote(lvgl_url, yp["lvgl"]["revision"])
    major = None
    for f in ("lv_version.h", "lvgl.h"):
        t = http_get(raw_url(lvgl_url, lvgl_sha, f))
        if t and (mm := re.search(r"#define\s+LVGL_VERSION_MAJOR\s+(\d+)", t)):
            major = int(mm.group(1))
            break
    return zephyr["url"], zephyr_sha, lvgl_url, lvgl_sha, major


def check_major(major, want, zmk_sha):
    if major is not None and major != want:
        warn(f"zmk@{zmk_sha[:10]} ships LVGL {major}.x but the widget sources target LVGL {want} "
             f"(SIM_LVGL_MAJOR); they will not compile, and neither would the real firmware build.")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cache", required=True)
    ap.add_argument("--keymap-repo", default="https://github.com/J-Jaywalker/alphred_kb")
    ap.add_argument("--west", default="", help="explicit west.yml (default: keymap repo's)")
    ap.add_argument("--zmk-rev", default="", help="override ZMK revision")
    ap.add_argument("--offline", action="store_true")
    ap.add_argument("--widget-lvgl", type=int, default=9,
                    help="LVGL major version the widget sources are written for (default 9); "
                         "only used for a mismatch warning")
    ap.add_argument("--refresh", action="store_true", help="re-resolve revisions even if fresh")
    ap.add_argument("--max-age", type=float, default=3600,
                    help="seconds a previous resolution stays fresh (default 3600)")
    a = ap.parse_args()

    cache = os.path.abspath(a.cache)
    os.makedirs(cache, exist_ok=True)
    deps_path = os.path.join(cache, "deps.json")
    prev = json.load(open(deps_path)) if os.path.exists(deps_path) else {}

    if a.offline:
        if not prev:
            raise SystemExit("--offline given but no previous fetch in cache")
        log("offline: using previously fetched dependencies")
        check_major(prev.get("lvgl_major"), prev.get("widget_lvgl_major", a.widget_lvgl),
                    prev.get("zmk_sha", "?"))
        return

    inputs = {"west_arg": a.west, "zmk_rev": a.zmk_rev, "widget_lvgl_major": a.widget_lvgl,
              "keymap_repo": a.keymap_repo, "sparse": [ZMK_SPARSE, ZEPHYR_SPARSE]}
    src = os.path.join(cache, "src")
    checked_path = os.path.join(cache, "deps.checked")
    try:
        age = time.time() - float(open(checked_path).read().strip() or 0)
    except (OSError, ValueError):
        age = -1
    if (not a.refresh and prev.get("inputs") == inputs and 0 <= age < a.max_age
            and all(stamp_ok(os.path.join(src, n), prev.get(f"{n}_sha", "-"))
                    for n in ("zmk", "zephyr", "lvgl"))):
        log(f"deps checked {int(age // 60)} min ago, reusing zmk@{prev['zmk_sha'][:10]} "
            f"(SIM_REFRESH=1 to re-check now)")
        check_major(prev.get("lvgl_major"), a.widget_lvgl, prev["zmk_sha"])
        return

    # 1. keymap repo
    kb = os.path.join(cache, "alphred_kb")
    if a.keymap_repo:
        if os.path.isdir(os.path.join(kb, ".git")):
            log("updating keymap repo")
            r = run(["git", "pull", "-q", "--ff-only"], cwd=kb, check=False, capture=True)
            if r.returncode != 0:
                log(f"git pull failed (offline?), using cached copy: {r.stderr.strip()}")
        else:
            log(f"cloning {a.keymap_repo}")
            run(["git", "clone", "-q", "--depth", "1", a.keymap_repo, kb])

    west = a.west or os.path.join(kb, "config", "west.yml")
    wp = west_projects(open(west, encoding="utf-8").read())
    zmk = wp.get("zmk", {"url": "https://github.com/zmkfirmware/zmk", "revision": "main"})
    zmk_url = zmk["url"]
    requested = a.zmk_rev or zmk["revision"]
    zmk_sha = ls_remote(zmk_url, requested)
    if zmk_sha is None:
        if prev:
            log("could not resolve ZMK revision (offline?); keeping previous fetch")
            return
        raise SystemExit(f"could not resolve ZMK revision {requested!r}")
    log(f"zmk@{requested}{' (ZMK_REV override)' if a.zmk_rev else ' (from west.yml)'} -> {zmk_sha[:10]}")

    zephyr_url, zephyr_sha, lvgl_url, lvgl_sha, major = lvgl_for_zmk(zmk_url, zmk_sha)
    want = a.widget_lvgl
    check_major(major, want, zmk_sha)

    shallow_fetch(os.path.join(src, "zmk"), zmk_url, zmk_sha, ZMK_SPARSE)
    shallow_fetch(os.path.join(src, "zephyr"), zephyr_url, zephyr_sha, ZEPHYR_SPARSE)
    shallow_fetch(os.path.join(src, "lvgl"), lvgl_url, lvgl_sha, ["src"])

    deps = {
        "west_yml": os.path.abspath(west),
        "zmk_requested": zmk["revision"], "zmk_url": zmk_url, "zmk_sha": zmk_sha,
        "zephyr_url": zephyr_url, "zephyr_sha": zephyr_sha,
        "lvgl_url": lvgl_url, "lvgl_sha": lvgl_sha, "lvgl_major": major,
        "widget_lvgl_major": want,
        "inputs": inputs,
    }
    # deps.json is a make prerequisite of the LVGL objects: only rewrite it when
    # something changed (the check time lives in deps.checked)
    if {k: prev.get(k) for k in deps} != deps or set(prev) != set(deps):
        with open(deps_path, "w") as f:
            json.dump(deps, f, indent=2)
    with open(checked_path, "w") as f:
        f.write(str(time.time()))
    log(f"ZMK {zmk_sha[:10]}, Zephyr {zephyr_sha[:10]}, LVGL {major}.x {lvgl_sha[:10]}")


if __name__ == "__main__":
    main()
