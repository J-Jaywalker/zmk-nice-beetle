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

## Central Beedly (replaces Luna) - checkpoint
- Pipeline: `sprite_assets/generate_beedly_left.py` (`make beedly_left`, part of `beedly_all`,
  `NO_ASEPRITE`/`ASEPRITE` as the other targets). Aseprite `-b --list-tags --data ... --format
  json-array --save-as frame_{frame}.png`; tags resolved by name (case-insensitive, direction
  honoured), missing tag = error. Frames normalised to black-on-transparent with ink = LIGHT
  pixels (the sprite is white on a dark `bg` layer; `--ink dark` for the other convention),
  rotated 90 degrees clockwise like the peripheral frames and the Luna `_90` images (checked in
  the sim: upright, not mirrored, same as the Aseprite canvas). Output
  `renders/beedly_left/{frame_<n>.png,tags.json}` and `assets/beedly_luna_frames.c/.h` (per-tag
  arrays + counts, deterministic, byte-identical reruns).
- Firmware: `widgets/beedly_slot.c/.h` owns the ONE lv_animimg (child of the canvas, created once,
  never deleted); luna.c (WPM), modifiers.c (explicit mods read in the display callback) and
  hid_indicators.c (caps bit only) just report state. Precedence Gui(Idle) > Alt > Ctrl > Shift >
  Caps > WPM; the object is only reconfigured when the chosen animation changes. Position
  `BEEDLY_SLOT_TOP 31` / `LEFT 18` (upright coordinates; text ends row 29, the WPM graph reaches
  row 64 at max). `assets/luna_images.c` is no longer compiled (file kept); the OLED variant
  (`boards/shields/nice_oled/widgets`, `CONFIG_NICE_EPAPER_ON=n`) still uses Luna, not ported.
  The old empty-label clipping bug of modifiers.c/hid_indicators.c is gone with the slot.
- New Kconfig (nice_epaper/Kconfig.defconfig, central only): `NICE_BEETLE_LUNA_WALK_WPM` 15,
  `_SPRINT_WPM` 70, `_IDLE_FRAME_MS` 480, `_WALK_FRAME_MS` 100, `_SPRINT_FRAME_MS` 100 (per frame
  = the old Luna cycle / 2). Mod/caps speed = old `*_LUNA_ANIMATION_MS` / 2 per frame.
  `NICE_OLED_WIDGET_{MODIFIERS,HID}_INDICATORS_LUNA=n` now disable those animations;
  `..._LUNA_ONLY_CAPSLOCK` has no effect any more (Num/Scroll Lock never animate).
- Inverted colours: the shared `PALETTE_INVERTED` (index 0 opaque black, index 1 transparent)
  makes black ink on the inverted screen's black background, i.e. invisible: verified in the sim
  with `CONFIG_NICE_VIEW_WIDGET_INVERTED=y` (CONF=corne.conf + that line), where the PERIPHERAL
  Beedly (beedly_frames.c, unchanged) shows nothing. The central frames use the new
  `PALETTE_INVERTED_WHITE_INK` (transparent / white) and show white Beedly there (checked).
  Pre-existing peripheral bug left as is (open issue).
- Tests: `sim/examples/beedly.txt` + `sim/tools/check_beedly.py`, both run by `make sim-test`
  (forced `--panel ideal` for the frame check). 29 window checks + 14 `expect`s, heap constant at
  2480 bytes through the run (peak C=4084, was 4600 with Luna).
- `make sim-watch`: `watch.py --art-target auto` maps the changed .aseprite to its target
  (beedly / beedly_dithered / beedly_left; others -> beedly_all). Exercised with a stand-in sim
  process: touching beedly_left.aseprite ran `make beedly_left` then `sim-build`.

## Bug: held Alt "switches between alt and ctrl" (fixed)
- Root cause: the ART. In beedly_left.aseprite the Alt tag's 2nd frame (index 11) was a copy of
  the Ctrl tag's 2nd frame (index 9) including the `keypress` layer (^ + "Ctrl" label); only the
  invisible `bg` cel differed. The firmware played Alt frames 10/11 correctly, so the screen
  alternated "ALT"/"Ctrl" every 150 ms. Logs showed one steady mods byte (no Ctrl bit).
  Fixed with an Aseprite batch Lua script: frame 12 (1-based) `keypress` cel replaced by a copy
  of frame 11's (Alt label); beetle layers untouched. Original kept nowhere in the repo but
  recoverable from git (`git show HEAD:sprite_assets/ase/beedly_left.aseprite`).
- Second, related: the window mapped PC keys by position, so PC Left Alt pressed position 37
  (`&kp LGUI` -> Idle) and PC Left Ctrl pressed 36 (`&kp LALT`). ui.c now maps the LEFT
  modifiers by function (active layer's plain `&kp` of that modifier, positional fallback; PC
  LWin falls back to 37). Right modifiers stay positional (RAlt/RCtrl = right thumb layer keys).
- Regression: `pckey` script command (ui.c `ui_pc_key()` -> the same `pc_key()` the SDL handler
  calls) + a PC-key section in beedly.txt; check_beedly.py fails on key-tag frames identical to
  another tag's (with the original art: 3+ FAIL lines, make sim-test exits non-zero).
- Real OS-level injection into the window (keybd_event from PowerShell) was unreliable from
  the agent session (window focus could not be held; one run delivered LAlt as key 37 = LGUI
  and lost focus after 0.36 s), so the window's SDL dispatch itself is verified only by reading
  ui.c; everything below it is exercised by `pckey`.

## Typing-only WPM
- `nice_epaper/widgets/wpm_counter.c/.h` (central, always compiled with the status screen): its
  own ZMK event `nice_beetle_wpm_changed` + `nice_beetle_wpm_get_state()`. luna.c and screen.c
  (number + graph) subscribe to it instead of `zmk_wpm_state_changed`.
- `CONFIG_NICE_BEETLE_WPM_TYPING_ONLY=y` (default): ZMK's wpm.c algorithm copied verbatim, with a
  filter on keyboard-page usages 0x04-0x28, 0x2B, 0x2C, 0x2D-0x38, 0x64, 0x54-0x63, 0x67, 0x85.
  `=n`: listener on `zmk_wpm_state_changed` re-raising it as `nice_beetle_wpm_changed`.
- `ZMK_WPM` is still selected (the fallback needs it, and other code may too). Its counter keeps
  running but no widget listens to its event, so nothing is double-raised or conflicting;
  the cost is one extra listener + 1 s timer.
- Sim: `SIM_Q_WPM` (`expect wpm`, window, log) = our counter; new `SIM_Q_WPM_ZMK` / `wpm_zmk` =
  ZMK's. `sim/examples/wpm.txt` (11 expects) in `make sim-test`.

## Beedly pet (Tamagotchi) - NOT built or run yet
- Written on a Mac without MSYS2: nothing below has been compiled (firmware or sim) or run.
- Firmware: `behaviors/behavior_beedly.c` (both halves), `widgets/beedly_menu.c` (central),
  `widgets/beedly_pet.c` (peripheral), `include/dt-bindings/zmk/beedly.h`,
  `dts/bindings/behaviors/zmk,behavior-beedly.yaml`, node in `nice_epaper.overlay`,
  `zephyr/module.yml` `dts_root: .`.
- Sim: gen_config.py adds `include/`, `dts/` (DTS include path) and `dts/bindings` (bindings, so
  `CONFIG_DT_HAS_ZMK_BEHAVIOR_BEEDLY_ENABLED` appears in Kconfig.dts); sim.mk compiles with
  `-Iinclude`; new queries `SIM_Q_PET_*` (sim_api.h, appended: ABI-compatible) answered by
  sim_main.c via the widgets' public getters; settings.c counts `beedly/...` saves.
- `make sim-test-pet KEYMAP=../alphred_kb/config/corne.keymap` runs `examples/pet.txt`.
  First run: check the `at 33500 expect pet.saves == 1` timing and the screenshots
  (`sim/build/out/pet/*.png`: menu box rows 64..117 on the left; live level digit, bars and
  hearts on the right).
- Central heap at boot should be unchanged (menu draws into the canvas, no objects): beedly.txt's
  `expect heap == 2480` still applies. The peripheral gains three canvas objects.

## Not verified / open issues
- Central Beedly on real hardware (flash): unverified, as everything else here.
- Peripheral Beedly is invisible with CONFIG_NICE_VIEW_WIDGET_INVERTED=y (palette, see above).
- `make sim-watch` art step (watcher -> `make beedly`) not exercised end to end; its parts were.
- Reboot model / settings persistence / ZMK_SLEEP (needs HAS_POWEROFF) / kscan debounce: as before.
- (fixed by the central Beedly slot) modifier/caps animations used to be children of an empty
  label and were clipped, i.e. never visible on hardware either.
- Window not DPI-aware; untested UI buttons as before.

## Commands
```
make sim                      # window (PANEL_EMU=memlcd, KEYMAP=..., SIM_ARGS=... optional)
make sim-watch                # window + rebuild/hot reload on save
make sim-test                 # headless smoke + central Beedly tests -> sim/build/out/
make sim-build SIM_OFFLINE=1  # incremental build, no network
sim/build/bin/sim.exe --help  # all options
```

## Gotchas
- Don't put `C:/msys64/usr/bin` or `mingw32/bin` before the Windows Python on PATH (no pyyaml there).
- sim.mk keeps tool paths in `C:/msys64/...` form; `cygpath -u C:/msys64` returns `/` under MSYS2 make.
- The agent Bash tool turns `\\n` inside heredocs into real newlines and mangles backslash-newline:
  write edit scripts with the Write tool.
- Kconfig: ZMK's corne Kconfig.defconfig wins over nice_epaper's (LV_Z_VDB_SIZE 64, LV_DPI_DEF 148).
