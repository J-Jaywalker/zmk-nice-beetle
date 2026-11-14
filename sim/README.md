# nice!view status screen simulator

A PC simulator of the two status screens of a ZMK split Corne running this
repo's widgets (`boards/shields/nice_oled/nice_epaper`), so screens,
animations and key-press reactions can be checked before flashing, and tested
automatically.

The **real firmware C sources** are compiled unmodified: this repo's widgets,
ZMK's keymap / behaviour engine / HID / WPM / battery / split / display code,
Zephyr 4.1's LVGL glue and LVGL 9.3 itself - the same ZMK, Zephyr and LVGL
revisions the real firmware build of alphred_kb uses (ZMK `main`). Only the hardware and radio layers
underneath are replaced (see [Accuracy](#accuracy-report)).

```
make sim                    # fetch, generate, build, open the window
make sim-watch              # window + rebuild/hot-reload on every save (animation work)
make sim-test               # headless scripted tests (smoke + central Beedly), PNG + GIF
make sim-test-pet KEYMAP=.. # Beedly pet scenario (needs a keymap with the BEEDLY layer)
make sim-deps               # check/install the MSYS2 toolchain (pacman)
make sim-clean              # remove sim/build (keeps the .sim_cache downloads)
```

Works from Git Bash, the MSYS2 shell and PowerShell with plain `make`, as long
as MSYS2's `make` (`C:\msys64\usr\bin`) and a Python 3 with `pyyaml` are on
PATH (put the Windows Python *before* `C:\msys64\usr\bin`: MSYS2's own python
has no pyyaml). The root `Makefile` includes `sim/sim.mk`.

## Requirements

* MSYS2 at `C:\msys64` (override with `SIM_MSYS2=...`) with
  `mingw-w64-i686-gcc` and `mingw-w64-i686-SDL2`; `make sim-deps` installs them
  with pacman. The 32-bit toolchain is used so struct sizes and the LVGL memory
  pool behave like on the 32-bit nRF52840.
* Python 3 with `pyyaml`, `git`, and network access for the first run.

The first `make sim` clones into the git-ignored `.sim_cache/`:
[J-Jaywalker/alphred_kb](https://github.com/J-Jaywalker/alphred_kb) (keymap
`config/corne.keymap`, `config/corne.conf`, `config/west.yml`) and sparse
checkouts of the ZMK, Zephyr and LVGL revisions that west.yml leads to.
Build output goes to the git-ignored `sim/build/`.

### Versions and board

alphred_kb's `west.yml` asks for ZMK `main`; the simulator resolves it exactly
like west would (ZMK `app/west.yml` -> Zephyr `v4.1.0+zmk-fixes`, LVGL 9.3
`f1db87ee98`). `ZMK_REV=<sha|branch|tag>` forces another revision; if that
ships a different LVGL major than the widgets target (`SIM_LVGL_MAJOR`,
default 9) a warning is printed, because neither the sim nor the firmware
would compile.

Resolving `main` needs the network (~40 s). It is done at most once an hour
(the result is cached in `.sim_cache/deps.json`); `SIM_REFRESH=1` re-checks
now, `SIM_OFFLINE=1` never touches the network.

ZMK `main` uses Zephyr's hardware model v2 and no longer knows the board name
`nice_nano_v2` that alphred_kb's `build.yaml` still uses: the real GitHub build
fails with an unknown-board error until `build.yaml` says `nice_nano//zmk`
(ZMK's Zephyr 4.1 blog post maps `nice_nano_v2` -> `nice_nano@2.0.0//zmk`).
The simulator maps the old name with a warning and builds for
`nice_nano@2.0.0/nrf52840/zmk`.

## Make variables

| Variable | Meaning |
|---|---|
| `KEYMAP=path` | keymap instead of alphred_kb's `config/corne.keymap` |
| `CONF=path` | Kconfig fragment instead of alphred_kb's `config/corne.conf` |
| `WEST=path` | west.yml used to choose the ZMK revision |
| `ZMK_REV=rev` | force a ZMK revision |
| `SIM_LVGL_MAJOR=9` | LVGL major of the widget sources (mismatch warning only) |
| `SIM_OFFLINE=1` | no network, use what is in `.sim_cache` |
| `SIM_REFRESH=1` | re-resolve ZMK `main` now instead of at most hourly |
| `SIM_WATCH_ART=auto` | make target `sim-watch` runs when `sprite_assets/ase/*` changes: `auto` = per file (`beedly_left.aseprite` -> `beedly_left`, ...), a target name = always that one, empty = never |
| `SIM_JOBS=N` | parallel compile jobs (default: number of CPUs) |
| `PANEL_EMU=ideal\|memlcd` | display model, default `ideal` (see below) |
| `SIM_ARGS="..."` | extra `sim.exe` arguments, e.g. `SIM_ARGS="--script my.txt"` |
| `SIM_MSYS2=C:/msys64` | MSYS2 root |

`make sim-run` starts the already built `sim/build/bin/sim.exe`.

## Iterating on animations

```
make sim-watch
```

builds, opens the window and keeps it open. Then just edit and save:

* `sprite_assets/ase/*.aseprite` -> runs the matching export target, then
  rebuilds: `beedly_ui.aseprite` -> `make beedly`, `beedly_ui_dithered.aseprite`
  -> `make beedly_dithered`, `beedly_left.aseprite` -> `make beedly_left`
  (central Beedly), any other file -> `make beedly_all`
  (`SIM_WATCH_ART=<target>` forces one target, `SIM_WATCH_ART=` none);
* anything under `boards/` (widgets, assets, Kconfig, overlay), `sim/shim`,
  `sim/kconfig` -> rebuilds.

The rebuild is `make sim-build SIM_OFFLINE=1`: no network, config generation
is skipped unless a config input changed, only changed objects recompile and
the two DLLs relink (about 2 s for an asset change). `sim.exe --watch` notices
the new `central.dll`/`peripheral.dll` and cold-boots that half from it
(like a reboot: the window, the other half, battery/USB/BLE state stay). Close
the window to stop. Without the watcher the same works by hand: keep
`make sim SIM_ARGS=--watch` open and run `make beedly` / `make sim-build` in
another terminal. `make beedly NO_ASEPRITE=1` skips Aseprite and rebuilds the
C file from the PNGs in `sprite_assets/renders/` (handy when you draw the
frames in another tool).

### Central Beedly (left half, replaces the Luna dog)

The central screen's Beedly comes from `sprite_assets/ase/beedly_left.aseprite`
(32x32). Each animation is an **Aseprite tag**: `Idle`, `Walk`, `Sprint`,
`Shift`, `Ctrl`, `Alt`, `CAPSLOCK` (case-insensitive; a missing tag stops the
export with an error, extra tags are ignored). Workflow:

1. edit `beedly_left.aseprite` in Aseprite (add/remove frames, move tag
   ranges - the C code needs no change, the frame counts are generated);
2. `make beedly_left` exports every frame + the tag list to
   `sprite_assets/renders/beedly_left/` (`frame_<n>.png` rotated to the panel
   orientation, `tags.json`) and writes
   `boards/shields/nice_oled/nice_epaper/assets/beedly_luna_frames.c/.h`
   (`NO_ASEPRITE=1` rebuilds them from the PNGs + `tags.json`);
3. `make sim` to look at it, or keep `make sim-watch` open: saving the
   `.aseprite` file runs steps 2 and the rebuild by itself;
4. `make sim-test` re-checks every animation (below).

The sprite is drawn light-on-dark (a dark `bg` layer): the light pixels are
Beedly. In the simulator, as on the panel, that ink uses the screen's
foreground colour like the text.

Which animation plays (`widgets/beedly_slot.c`, one LVGL object):
Gui held -> Idle (there is no Gui tag; Gui used to make Luna sit), else Alt ->
Alt, else Ctrl -> Ctrl, else Shift -> Shift (first match wins, the old Luna
priority), else Caps Lock on -> CAPSLOCK, else by WPM: below
`CONFIG_NICE_BEETLE_LUNA_WALK_WPM` (15) Idle, below
`CONFIG_NICE_BEETLE_LUNA_SPRINT_WPM` (70) Walk, otherwise Sprint. Releasing a
key or turning Caps Lock off falls back down this list, normally to the WPM
animation. Num Lock / Scroll Lock show nothing. Frame times:
`CONFIG_NICE_BEETLE_LUNA_{IDLE,WALK,SPRINT}_FRAME_MS` (480/100/100 ms per frame)
and the old `..._MODIFIERS_INDICATORS_LUNA_ANIMATION_MS` /
`..._HID_INDICATORS_LUNA_ANIMATION_MS` (300 ms per two frames).

`make sim-test` runs `sim/examples/beedly.txt` after the smoke test, dumps
every frame (`sim/build/out/beedly/frames/`, plus `beedly.gif` and
screenshots per state) and runs `sim/tools/check_beedly.py`. Its
`#@beedly <from>..<to> <tag>` lines say which tag must be on screen in that
window; the checker cuts the 32x32 slot out of every dumped frame and
requires a pixel-exact match with a frame of that tag (wrong animation,
stale/overlapping image, rotation, mirroring, clipping and position all
fail), every frame of the tag to show up, the mean frame time to match the
configuration, and the rows just above/below the slot to be empty (no
overlap with the battery text / WPM graph). The scenario covers idle, walk
(auto typer 40 WPM), sprint (120 WPM), Shift/Ctrl/Alt/Gui held alone and
together, Caps Lock on/off (and Shift over Caps Lock), releases back to the
WPM animation, the window's PC Left Alt/Ctrl/Shift/Win keys (`pckey`), and a
constant LVGL heap (no leaked objects). The checker also fails when a
Shift/Ctrl/Alt/CAPSLOCK frame is identical to another tag's frame (the art
once had a Ctrl frame copied into the Alt tag, so held Alt flipped between
"ALT" and "Ctrl"); `make beedly_left` warns about the same thing.
`--log FILE` records, at debug level, every PC key and the Corne key it
pressed (`ui: PC Left Alt -> key 36 (LALT)`), window focus loss, and every
Beedly animation change with the modifier byte (`beedly: mods 0x04 ...`).
The WPM shown (number, graph, Idle/Walk/Sprint) comes from
`widgets/wpm_counter.c`. It uses ZMK's algorithm (key releases, 5 keys per
word, 1 s update, 5 s window) but only counts typing keys: HID keyboard page
letters/digits/symbols, Space, Tab, Enter, and keypad digits/operators/Enter.
Modifiers, Backspace/Delete, Esc, navigation, F-keys, lock keys and consumer
keys are not counted. `CONFIG_NICE_BEETLE_WPM_TYPING_ONLY=n` passes ZMK's
counter through instead. `sim/examples/wpm.txt` (part of `make sim-test`)
checks that 25 letters, or a mix of letters, Space, Tab and Enter, give
60 WPM while Backspace, Esc, modifiers (tapped, held, home-row mods held),
arrows, F-keys and layer keys give 0, comparing against `wpm_zmk`.

### Beedly pet

The `&beedly` behaviour, the left-screen menu and the right half's pet (see
the main README) are compiled like the rest: `behaviors/behavior_beedly.c`
into both DLLs, `widgets/beedly_menu.c` into the central, `widgets/beedly_pet.c`
into the peripheral. The node comes from `nice_epaper.overlay`; the sim adds
this repo's `include/` and `dts/` to the devicetree include path and
`dts/bindings` to the binding directories, as Zephyr does for a module with
`dts_root: .` (`zephyr/module.yml`), and `include/` to the compiler's path.
The central's `zmk_split_central_invoke_behavior()` goes through the sim's
split relay to the peripheral's real command handler.

The menu needs a keymap with a `BEEDLY` layer and the `&beedly` bindings.
`make sim` clones alphred_kb's default branch, which does not have them yet;
point the sim at a local checkout instead (the `tamagotchi_behaviour` branch):

```
make sim KEYMAP=../alphred_kb/config/corne.keymap [CONF=../alphred_kb/config/corne.conf]
make sim-test-pet KEYMAP=../alphred_kb/config/corne.keymap
```

(`WEST=../alphred_kb/config/west.yml` also takes the ZMK revision from that
checkout; the sim always compiles this repo's working tree, whatever
west.yml names for zmk-nice-beetle.) In the window: hold the BT key (right
thumb, outer) and press P to open the menu, I / K move the cursor, the key
right of `/` (Enter on the base layer) confirms.

`sim/examples/pet.txt` (`make sim-test-pet`) opens the menu from ALPH-MAC,
checks the cursor clamps at both ends, FEEDs until the food bar is full
(animation running, a press during it ignored, XP), checks the one-shot save
after 30 s, that FEED on a full bar changes nothing (not even the unsaved
flag), PETs to full and level 2, SAVEs (written at once, menu closed, back on
ALPH-MAC, the pending timer save replaced) and that `&to` on the BEEDLY layer
closes the menu; PNGs go to `sim/build/out/pet/`. **It has not been run
yet** (written without a Windows machine): treat a first failure as a
possibly wrong expectation as much as a bug. `expect` queries:
`pet.menu`, `pet.cursor` (central), `pet.hearts`, `pet.food`, `pet.xp`,
`pet.level`, `pet.dirty`, `pet.anim`, `pet.saves` (peripheral; `pet.saves`
counts `settings_save_one("beedly/...")` calls in the sim's settings store).
Not covered: settings are not persisted across sim reboots (so loading a
record is not exercised), deep sleep (not active in the sim), and the
split-link-down path (all menu keys are on the right half, so with the link
down they never reach the central).

LVGL advances animations from its ~33 ms timer, so a single 100 ms frame can
be on screen for 40..160 ms (as with the old Luna); the average is exact.

Notes: the simulator runs each DLL from a copy in `sim/build/bin/shadow/`, so
builds can overwrite the DLLs while it runs. A change that relinks `sim.exe`
itself (host code, keymap layout) is logged as "restart the simulator". For
frame-by-frame checks use headless mode:
`sim/build/bin/sim.exe --headless --duration 5000 --frames out/` writes every
distinct frame with its virtual time (animation timing is the firmware's own:
the LVGL timers/work queues run on the simulated kernel clock).

## The window

Central (left half) screen on the left, peripheral (right half) on the right,
each with its own battery, USB/charging and split-link state. The middle shows
the current layer, WPM, modifiers, output (USB/BLE), BLE profile and host
state, the computer's caps/num/scroll lock state, activity state and the text
the simulated computer has received. Below is a clickable 42-key Corne showing
the active layer's bindings.

* **Mouse:** left-click and hold a key to press it; right-click latches a key
  down (for example a layer key) until right-clicked again.
* **PC keyboard** (by physical position, layout independent):
  `Tab`/`Esc`/`` ` ``, `Q`..`T`, `Y`..`P`, `Backspace`/`[` = top row;
  `CapsLock`, `A`..`G`, `H`..`;`, `'` = home row; `LShift`, `Z`..`B`, `N`..`/`,
  `RShift` = bottom row; thumbs `LCtrl` `LAlt`/`LWin` `Space` | `Enter` `RAlt`
  `RCtrl`/`Menu`. Exception: the LEFT modifier keys `LShift` `LCtrl` `LAlt`
  `LWin` press the key that is plain `&kp <that modifier>` on the active
  layer (alphred_kb ALPH-WIN: PC Left Alt -> `&kp LALT` on position 36, PC
  Left Ctrl -> `&kp LCTRL` on 38, PC Win -> `&kp LGUI` on 37), so Alt on the
  PC is Alt on the keyboard; only when the layer has no such key the position
  above is used. Hold-tap modifiers (`&hm LEFT_ALT S`) are never used for this.
* **Controls** (buttons, and keys): F1/F2 central battery -/+10, F3/F4
  peripheral battery -/+10, F5/F6 central/peripheral USB none -> powered
  (charging) -> hid, F7 next BLE profile, F8 host present for the active
  profile, F9 bond/unbond it, F10 split link, F11 speed (1x..30x), F12
  screenshot (`sim/build/screenshots/`), Pause, Insert = skip 30 s (reach idle),
  Home = output USB/BLE, PageUp/PageDown/End = the computer's caps/num/scroll
  lock, Delete = auto-typer off/40/80/120 WPM, Scroll Lock = help overlay.
  F-keys and these control keys never reach the simulated keyboard.

Battery changes only show up when that half's ZMK `battery.c` samples again,
exactly like on hardware: every `CONFIG_ZMK_BATTERY_REPORT_INTERVAL` (60 s)
while active, not at all while idle, and immediately when the half wakes up.
Each half has its own activity state: the central wakes from keys on either
half (they all pass through it), the peripheral only from its own keys.

## Headless mode and scripts

```
sim/build/bin/sim.exe --headless --script sim/examples/smoke.txt \
    --png out.png --gif out.gif --frames out_frames/
```

| Option | |
|---|---|
| `--headless` | no window; run as fast as possible and exit |
| `--script FILE` | timed input (text, or JSON if `.json` / starts with `[`); also works with the window |
| `--duration MS` | virtual run time (default: 1 s after the script finishes, or 10 s) |
| `--png FILE` | final screens as PNG |
| `--gif FILE` | animated GIF of every screen change, with real timing |
| `--frames DIR` | every distinct frame as `DIR/frame_<ms>.png` |
| `--scale N` | image scale (default 2) |
| `--panel ideal\|memlcd`, `--memlcd-response-ms N`, `--rotate DEG` | display model / view |
| `--battery-central N`, `--battery-peripheral N` | initial battery (default 80 / 60) |
| `--usb-central S`, `--usb-peripheral S` | initial USB state `none\|powered\|hid` |
| `--split-off`, `--split-latency MS` | start with the link down / delay split packets |
| `--log-level 0..4`, `--log FILE`, `--quiet` | logging (firmware `LOG_*` included) |
| `--watch` | window only: reload a half when its DLL is rebuilt |

Exit code: 0 ok, 1 if any `expect` failed, 2 usage/script error, 3 DLL load
failure. Images show both screens side by side, rotated upright.

### Script syntax

Text form, one command per line (see `sim/examples/smoke.txt`):

```
# comment                 ('#' at line start or after whitespace, outside "quotes")
at <time> [command]       move the cursor to <time> (absolute virtual ms) and run the command there
wait <time>               cursor += time   (alias: sleep)
+<time> <command>         cursor += time, then the command
<command>                 at the current cursor (starts at 0)
<time> = 1500 | 1500ms | 1.5s
```

| Command | |
|---|---|
| `press <pos>` / `release <pos>` | matrix position 0..41 (row by row, left to right; 36..41 = thumbs) |
| `pckey <key> down\|up` | a PC keyboard key through the window's mapping (`lalt`, `lctrl`, `lshift`, `lgui`, `ralt`, `rctrl`, `rshift` or an SDL key name with `_` for spaces, e.g. `Left_Alt`, `Space`) |
| `tap <pos> [hold=40]` | press now, release after `hold` ms; the cursor moves on to release + 30 ms |
| `type <text>` | taps letters, `, . / ; '` and space by QWERTY position (40 ms hold + 40 ms gap each); other characters are skipped with a warning. What comes out depends on the keymap |
| `battery <central\|peripheral\|both> <0..100>` | also `c`/`p`/`left`/`right` |
| `usb <half\|both> none\|powered\|hid` | `powered` = charger only, `hid` = enumerated by a computer |
| `ble select <n>` / `ble host <n> on\|off` / `ble bond <n> on\|off` | BLE profile 0..4; "host" = a computer for that profile is in range |
| `output usb\|ble` | what `&out OUT_USB/OUT_BLE` does |
| `split on\|off` | split link between the halves |
| `leds none` / `leds [caps] [num] [scroll]` | the computer's lock-key state (sent as an LED report) |
| `autotype <wpm>\|off` | auto typer (taps q..p across both halves) |
| `screenshot <file.png>` | PNG of both screens |
| `log <text>` | print a message |
| `end` | stop the run here (`quit`, `exit`) |
| `expect <query> <op> <value>` | check state; ops `== != < <= > >=` (`typed`: `== != contains`) |

`expect` queries: `layer` (number or layer name), `layer_state`, `wpm` (the
WPM the central screen shows: typing keys only, see below), `wpm_zmk` (ZMK's
own counter, every key),
`mods` (number or `lgui+lshift`...), `battery`, `usb`, `split`, `writes`,
`heap`, `heap_peak` (each optionally `.central`/`.peripheral`; default
central), `profile`, `connected`, `bonded`, `transport` (`usb`/`ble`/`none`),
`leds` (what the firmware has received: `caps+num`...), `activity`
(`active`/`idle`/`sleep`), `typed` (text received by the computer), and the
Beedly pet's `pet.*` (see [Beedly pet](#beedly-pet)). Results
are printed as `PASS`/`FAIL` with the script line. Unknown commands, queries
or values are reported (file:line) before the run starts.

JSON form (file ends in `.json` or starts with `[`; see
`sim/examples/layers.json`): an array of either strings (one text line each)
or objects `{"at": ms, "wait": ms, "cmd": "...", "args": [...]}` (all keys
optional; `args` are appended to `cmd`), e.g.
`[{"at":1000,"cmd":"tap","args":[13,250]}, "wait 500", {"cmd":"expect wpm > 0"}]`.

## Display models

Pluggable post-processing between the firmware's display driver and what is
shown (`sim/host/panel.c`); `PANEL_EMU` / `--panel` selects it:

* `ideal` (default): every write is shown instantly and exactly, 1 = white.
  Best for pixel-exact tests.
* `memlcd`: the overlay's panel is a **Sharp memory LCD** (`sharp,ls0xx`,
  the nice!view), not e-paper. This model:
  * makes each write occupy the display thread for its real SPI transfer time,
    computed like Zephyr's `ls0xx` driver sends it (command byte, per line
    address + 20 data bytes + dummy byte, trailer byte) at the overlay's
    `spi-max-frequency` (1 MHz: about 12 ms for a full 68-line frame), so
    back-to-back flushes queue up as on hardware;
  * latches lines onto the glass one by one during the transfer, so a frame
    caught mid-update shows the same tearing the real panel can show;
  * keeps pixel state between writes (memory-in-pixel: no refresh, no decay);
  * uses an approximate reflective grey/white palette (not measured);
  * `--memlcd-response-ms N` adds a linear liquid-crystal response time; the
    default 0 is deliberate because no datasheet value was available to
    calibrate it;
  * **VCOM**: this overlay has neither `extcomin-gpios` nor (Zephyr 4.1)
    `serial-vcom-inversion`, and the driver's write command never sets the
    serial VCOM bit, so the firmware never inverts VCOM.
    That has no visible short-term effect and is not drawn; any long-term
    DC-bias degradation of a real panel is not modelled.

Chip-select setup/hold times and CPU time spent rendering are not included.

## Accuracy report

What is **real** (compiled from the original sources, unmodified):

* This repo's nice_epaper widgets and status screens (central and peripheral),
  their assets and fonts, with the real Kconfig values: Zephyr 4.1's
  kconfiglib over ZMK's `app/Kconfig` + `app/module` Kconfig + LVGL's Kconfig,
  the HWMv2 board (`nice_nano@2.0.0/nrf52840/zmk`: ZMK's `zmk` variant
  extending `app/module/boards/nicekeyboards/nice_nano`, its
  `..._2_0_0_defconfig`), ZMK `prj.conf`, the shield `.conf`s, `corne.conf`
  and the studio snippet, merged in Zephyr 4.1's order. The devicetree is real
  for everything the firmware reads (alphred_kb keymap, Corne matrix
  transform, this repo's overlay = the `sharp,ls0xx` node, processed by
  Zephyr's `gen_edt.py`/`gen_defines.py`); the nRF52840/nice!nano/adapter
  pin-level nodes are replaced by a small stub board (kscan mock, stub SPI
  bus, `vbatt`).
* LVGL 9.3 with Zephyr 4.1's LVGL glue (`lvgl.c`, `lvgl_display.c`,
  `lvgl_display_mono.c` = the I1 -> MONO01 conversion the ls0xx driver gets,
  `lvgl_mem.c`), compiled with the same `LV_CONF_PATH`/`LV_CONF_INCLUDE_SIMPLE`
  defines and the real `CONFIG_LV_Z_MEM_POOL_SIZE` pool (8192 bytes with this
  config) on Zephyr's `sys_heap` (`lib/heap`). LVGL's built-in printf provides
  `snprintf`/`vsnprintf` for all firmware code, as it does on the nRF52.
* ZMK: keymap, all behaviours the keymap pulls in (hold-tap/home-row mods,
  combos, mod-morph, sticky keys, caps word, key repeat, momentary/toggle/to
  layers, outputs, Bluetooth, ...; macros are built when a keymap uses them),
  HID report building, `hid_listener`, endpoints, WPM, activity/idle,
  `battery.c`, `usb.c`, `hid_indicators.c`, split `central.c`/`peripheral.c`,
  `display/main.c` and ZMK's `main.c`. Event listeners run in the real
  link order.
* Two separate "MCUs" (`central.dll`, `peripheral.dll`), each with its own
  LVGL, kernel clock and globals, run in 1 ms lock-step. Right-half keys are
  scanned by the peripheral and travel to the central through ZMK's real split
  code; the peripheral's screen gets its state the same way it does on
  hardware.

What is **replaced or approximated**:

* Kernel: a cooperative shim (32768 Hz ticks, Zephyr's own time conversions;
  tracing is compiled out, `CONFIG_LV_Z_USE_OSAL` is off in this config so
  LVGL runs single-threaded on the display work queue as on hardware).
  Work items and timers run to completion in zero virtual time; **CPU time is
  not modelled** (on the nRF52 canvas rotation and rendering take
  milliseconds), so animations can look smoother/more punctual than hardware.
* Display driver: a stand-in for Zephyr's `ls0xx` with the same API contract;
  bus timing only with `PANEL_EMU=memlcd` (see above).
* BLE (`ble.c`, `hog.c`) is a behavioural model: 5 profiles, profile 0 bonded
  to a host that is in range at boot, connect ~0.4 s after advertising starts
  (1.5 s when pairing an open profile), the same events ZMK's `ble.c` raises.
  No radio timing, RSSI, or connection parameter behaviour.
* Split transport (`split/bluetooth/*`) is replaced by a packet relay between
  the two DLLs (instantaneous by default, `--split-latency`). Peripheral
  battery events are dropped like the BLE transport does (it uses BAS, and
  `ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING` is off).
* USB (`usb_hid.c`, Zephyr USB stack) is a plug-state model (`none`,
  `powered` = charger only, `hid` = enumerated); `usb.c` on top is real.
  "Charging" is what the firmware infers from USB power.
* Battery: a fuel-gauge stand-in returning the chosen percentage; no ADC
  noise or voltage curve effects.
* Physical layouts (`physical_layouts.c`) and kscan: keys are injected as
  matrix positions; **kscan debounce** (`CONFIG_ZMK_KSCAN_DEBOUNCE_*`) and
  physical-layout switching are not simulated.
* Settings are not persisted: each run (and each simulated reboot) starts
  fresh, so BLE bonds other than the default profile 0 are lost on reboot.
* Reboot (`&sys_reset`): the half's DLL is unloaded and reloaded (cold boot,
  panel cleared, keys still held are seen by the first scan). The split link
  stays up instead of dropping and reconnecting. `&bootloader` stops that
  half's firmware (the sim keeps showing its last image; what the real
  panel shows then depends on the bootloader) until the simulator is
  restarted; there is no bootloader/UF2 drive model.
* The computer: a simple OS model types the HID reports into a text buffer
  and toggles caps/num/scroll lock on the corresponding usages, then sends the
  LED report back to the keyboard (like a real host).
* Kconfig symbols that need real hardware drop out silently, as a Kconfig
  dependency would: **`CONFIG_ZMK_SLEEP` (deep sleep after the 1 h
  `ZMK_IDLE_SLEEP_TIMEOUT`) is not active in the simulator** (it depends on
  `HAS_POWEROFF`); the BT TX power / GATT options are irrelevant here.
* ZMK Studio RPC is off in this config and not simulated.
* Linking: the firmware links with `--gc-sections`, the DLLs do not, so
  `lv_circle_buf.c` (missing from Zephyr's LVGL source list but referenced by
  the otherwise-unused `lv_iter.c`) is added; heap statistics
  (`CONFIG_SYS_HEAP_RUNTIME_STATS`) are compiled into the heap code to report
  the LVGL pool usage, which adds a few bytes of heap bookkeeping.
* x86-32 vs. Cortex-M4: same pointer/int sizes, but struct padding and
  floating-point details may differ slightly.

### NULL-pointer reads

On the nRF52840 address 0 is readable flash (the vector table), so a firmware
read through a NULL pointer silently returns junk; on Windows it is an access
violation. The LVGL 8 widgets did this during screen init (`as_zmk_*()` on a
NULL event in `screen.c`/`luna.c`/`hid_indicators.c`, LVGL 8's
`lv_canvas_transform()`). The LVGL 9 port fixed these: the widgets now fall
back to `zmk_battery_state_of_charge()`, `zmk_wpm_get_state()` and
`zmk_hid_indicators_get_current_profile()` when there is no event, and
`lv_canvas_transform()` is no longer used; a `make sim-test` run logs no NULL
read. `sim/shim/src/null_page.c` stays as a safety net: it
emulates loads from the first 64 KiB as reading 0 and logs each faulting site
once ("firmware read through a NULL pointer") instead of crashing. Hardware
reads vector-table bytes, so treat any such log line as a real bug to fix.

## Layout of `sim/`

```
sim.mk                 build rules (included from the root Makefile)
host/                  sim.exe: main.c (lock-step, routing, computer model,
                       headless), ui.c (window), panel.c (display models),
                       script.c, image.c (PNG/GIF), sim_api.h (host <-> MCU ABI)
shim/                  Zephyr/ZMK replacements compiled into each MCU DLL
kconfig/               Kconfig stubs for things the real tree gets from the SoC
tools/                 fetch_deps.py, gen_config.py, setup_toolchain.py,
                       cmake_sources.py, watch.py (make sim-watch)
examples/              smoke.txt, beedly.txt, wpm.txt (make sim-test), layers.json,
                       pet.txt (make sim-test-pet)
tools/check_beedly.py  frame checker for beedly.txt
```
