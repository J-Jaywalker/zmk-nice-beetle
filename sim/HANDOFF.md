# Simulator handoff (checkpoint: LVGL 9 / Zephyr 4.1 port done, `make sim-watch` added)

User-facing docs: `sim/README.md` (usage, animation loop, script syntax, accuracy report). This file is
for whoever continues the work. Nothing has been committed.

## Status
- Builds against what alphred_kb's `west.yml` resolves to: ZMK `main` 4dd95d7840, Zephyr
  `v4.1.0+zmk-fixes` 10ba6d0cb3, LVGL 9.3 f1db87ee98. The LVGL 8 fallback pin is gone;
  `SIM_LVGL_MAJOR` defaults to 9 and only produces a mismatch warning.
- `make sim-test` (25 expectations) and `sim/examples/layers.json` pass with the ported widgets, from Git
  Bash and PowerShell (plain `make`, root `Makefile`). Expectations unchanged; the old transport
  failures came from ZMK main's new `ZMK_TRANSPORT_NONE = 0` (sim_main.c now maps to the host's
  0 USB / 1 BLE / -1 none).
- Fast loop: `make sim-watch` (sim/tools/watch.py + `sim.exe --watch`), ~2 s from save to reloaded half.
  No-op `make sim-build` ~1-2 s (fetch fresh-check 0.2 s, gen skip 0.1 s, sub-make 0.7 s).

## Porting decisions
- fetch_deps.py: sparse paths for ZMK main (`app/boards`, `app/module`, `app/keymap-module`) and
  Zephyr `lib/heap`, `drivers/display` (reference). Network resolution at most hourly
  (`deps.checked` timestamp; `--refresh`/`SIM_REFRESH=1`); `deps.json` is rewritten only when a
  revision/input changes (it is a make prerequisite of the LVGL objects).
- Board (gen_config.py `board_target`): build.yaml's `nice_nano_v2` is mapped to
  `nice_nano@2.0.0/nrf52840/zmk` with a warning. ZMK main does NOT accept `nice_nano_v2`, so the
  user's real GitHub build fails until build.yaml says `nice_nano//zmk` (outside this repo).
- Kconfig: sim/kconfig/Kconfig.zephyr mirrors Zephyr 4.1 HWMv2: generated `boards/Kconfig.*`
  (osource per board dir: base + zmk variant), `Kconfig.board.v2` (BOARD_NICE_NANO,
  BOARD_NICE_NANO_NRF52840_ZMK, BOARD_REVISION...), Kconfig.modules = LVGL (`if LVGL`) + ZMK
  `app/module`. Defconfig found like `zephyr_file(CONF_FILES ... DEFCONFIG)`. Merge order as Zephyr
  4.1: board defconfig, `app/prj.conf`, shield confs (+ shield `boards/<board>.conf`), user
  `corne.conf`, snippet conf, `sim/kconfig/sim.conf`.
- DT: still the stub board (kscan mock, stub SPI bus, vbatt) + real corne transform, overlay,
  keymap. Zephyr 4.1 split: `gen_edt.py` then `gen_defines.py --edt-pickle`. Generated headers now
  live in `gen/<role>/include/generated/zephyr/` (`<zephyr/autoconf.h>`, `<zephyr/devicetree_generated.h>`);
  `gen/zephyr/version.h` generated from zephyr/VERSION; syscall wrappers for both `<syscalls/x.h>`
  (ZMK) and `<zephyr/syscalls/x.h>`.
- Compile flags as Zephyr's LVGL CMake: `LV_CONF_INCLUDE_SIMPLE`, `LV_CONF_PATH="lv_conf.h"` (needed:
  LVGL's Kconfig sets LV_CONF_SKIP). `LV_LVGL_H_INCLUDE_SIMPLE` dropped (Zephyr doesn't define it).
- `sim/shim/include/sim_printf_override.h` (force-included): LVGL builtin sprintf defines the
  firmware's snprintf/vsnprintf (as on nRF52); renamed to sim_fw_* to dodge mingw's static inlines.
- New shims: `zephyr/tracing/tracing.h` (no-op macros; real one drags in kernel_structs.h),
  `zephyr/net_buf.h`, `LOG_*_ONCE`. Endpoint API renames (`zmk_endpoint_get_selected`,
  `zmk_endpoint_set_preferred_transport`).
- sys_heap moved to `lib/heap` (heap.c, heap_stats.c, heap_info.c; RUNTIME_STATS forced for the
  pool report). `lv_circle_buf.c` added to the LVGL lib (firmware gets away via --gc-sections).
- Display: unchanged ls0xx contract; Zephyr 4.1's `lvgl_display_mono.c` does the I1 -> MONO01
  (LSB-first) conversion, so fake_display.c needed no format change.
- Build speed: LVGL objects don't include their .d files (60k header stats = 8 s per no-op on
  Windows); they depend on autoconf.h + deps.json instead. Object names carry a path hash, so stale
  .d files can't hit a different source. gen_config.py skips when its input signature (mtime/size of
  keymap, conf, build.yaml, deps.json, sim/tools, sim/kconfig, boards/** config files, args) is
  unchanged (`--force`). Sub-make runs `-j$(NUMBER_OF_PROCESSORS)`.
- Hot reload: host loads each DLL from `sim/build/bin/shadow/<name>.<pid>.<n>.dll`; `--watch` polls
  DLL mtimes every 250 ms, reloads after 500 ms of stability via the reboot path (new copy loaded
  before the old is freed; failure keeps the old firmware). sim.exe links to `.new` then renames.

## Verified
- `make sim-test` Bash + PowerShell: 25/25; layers.json passes. PNGs: rotation 270 upright text,
  central = Luna + WPM graph + layer name (ARROWS / ALPH-WIN) + battery; peripheral = Beedly + "BEEDLY : LVL 5".
  Headless `--frames`: peripheral Beedly frame changes every ~960 ms, Luna animates.
- `sim.exe --watch` (window) reloaded both halves after `make sim-build`; `make sim-watch` end to end:
  touching sim/shim file -> rebuild 1.9 s -> both halves reloaded.
- LVGL heap peak C=4600 / P=3204 of the 8192-byte pool (A's Kconfig now sets 8192).

- Coordinator integration pass: `make sim-test` (Bash + PowerShell, ideal and PANEL_EMU=memlcd, PNG
  checked), layers.json, window start, `--frames` (Beedly alternates every ~960 ms = 1920 ms / 2
  frames), incremental rebuild after touching beedly_frames.c 1.6 s, no "NULL pointer" log lines
  (widgets' NULL-event reads fixed in the port). `make beedly` / `beedly_all` with Aseprite now
  works under MSYS2 make from Git Bash and from PowerShell (generate_lvgl_c_arrays.py restores
  APPDATA/LOCALAPPDATA/USERPROFILE/TEMP/TMP, which MSYS2 make started from Git Bash drops; without
  them Aseprite never exits).

## Not verified / open issues
- `make sim-watch` art step (watcher -> `make beedly`) not exercised end to end; its parts were.
- Reboot model / settings persistence / ZMK_SLEEP (needs HAS_POWEROFF) / kscan debounce: as before.
- Modifier/caps Luna (modifiers.c, hid_indicators.c) are created as children of an empty label at
  offset (100,15), so they are clipped by the label and don't show. Pre-existing: LVGL 8.3 clips
  children the same way (no LV_OBJ_FLAG_OVERFLOW_VISIBLE), the port didn't change the structure.
  Not changed because it would alter what the firmware shows; a widget design fix if wanted.
- Window not DPI-aware; untested UI buttons as before.

## Commands
```
make sim                      # window (PANEL_EMU=memlcd, KEYMAP=..., SIM_ARGS=... optional)
make sim-watch                # window + rebuild/hot reload on save
make sim-test                 # headless smoke test -> sim/build/out/
make sim-build SIM_OFFLINE=1  # incremental build, no network
sim/build/bin/sim.exe --help  # all options
```

## Gotchas
- Don't put `C:/msys64/usr/bin` or `mingw32/bin` before the Windows Python on PATH (no pyyaml there).
- sim.mk keeps tool paths in `C:/msys64/...` form; `cygpath -u C:/msys64` returns `/` under MSYS2 make.
- The agent Bash tool turns `\\n` inside heredocs into real newlines and mangles backslash-newline:
  write edit scripts with the Write tool.
- Kconfig: ZMK's corne Kconfig.defconfig wins over nice_epaper's (LV_Z_VDB_SIZE 64, LV_DPI_DEF 148).
