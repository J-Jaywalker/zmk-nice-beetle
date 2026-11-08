# Simulator build rules (included from the top-level Makefile).
#
#   make sim          fetch deps, generate config, build, run the simulator window
#   make sim-build    same, without running (incremental: only what changed)
#   make sim-run      run the already-built simulator
#   make sim-watch    build, open the window, then rebuild + hot-reload on every
#                     save of a widget/asset/config file (and run `make beedly`,
#                     `beedly_dithered` or `beedly_left` when the matching
#                     sprite_assets/ase/*.aseprite changes) - see sim/README.md
#   make sim-test     headless scripted smoke test (sim/examples/smoke.txt)
#   make sim-deps     check / install the MSYS2 toolchain (gcc, SDL2) via pacman
#   make sim-clean    remove simulator build output (keeps the .sim_cache downloads)
#   make sim-distclean  also remove .sim_cache
#
# Variables (make sim VAR=value):
#   KEYMAP=path        keymap to use instead of alphred_kb's config/corne.keymap
#   CONF=path          Kconfig fragment instead of alphred_kb's config/corne.conf
#   WEST=path          west.yml used to pick the ZMK revision
#   ZMK_REV=rev        force a ZMK revision (sha/branch/tag).  Default: the one
#                      west.yml names (alphred_kb: main = Zephyr 4.1 / LVGL 9)
#   SIM_LVGL_MAJOR=9   LVGL major the widget sources target (mismatch warning)
#   SIM_OFFLINE=1      don't touch the network, use what is in .sim_cache
#   SIM_REFRESH=1      re-resolve ZMK main now (otherwise re-checked at most
#                      hourly; see sim/tools/fetch_deps.py --max-age)
#   PANEL_EMU=ideal|memlcd  display model: ideal 1-bit panel (default) or Sharp
#                      memory LCD (ls0xx) bus timing / line-by-line update
#   SIM_WATCH_ART=auto make target sim-watch runs when the sprite sources
#                      change: auto = per file (beedly_left.aseprite ->
#                      beedly_left, ...), a target name = always that one,
#                      empty = never
#   SIM_ARGS="..."     extra arguments for sim.exe (see sim/README.md), e.g.
#                      SIM_ARGS="--script sim/examples/smoke.txt --headless --gif out.gif"
#   SIM_MSYS2=C:/msys64  MSYS2 install root

SIM_DIR := sim
SIM_BUILD := sim/build
SIM_GEN := $(SIM_BUILD)/gen
SIM_CACHE := .sim_cache

SIM_MSYS2 ?= C:/msys64
SIM_ENV ?= mingw32
SIM_PY ?= python

# Tool paths stay in Windows "C:/..." form: they are passed to native
# programs (python, gcc) and also work as commands in msys make's sh.  Only
# PATH needs the POSIX form; cygpath of the MSYS2 root itself would give "/"
# when make runs under that same MSYS2, so convert the bin dir directly.
SIM_TOOLBIN := $(SIM_MSYS2)/$(SIM_ENV)/bin
SIM_TOOLBIN_POSIX := $(shell cygpath -u '$(SIM_TOOLBIN)' 2>/dev/null || echo '$(SIM_TOOLBIN)')
SIM_CC := $(SIM_TOOLBIN)/gcc.exe
SIM_AR := $(SIM_TOOLBIN)/ar.exe

SIM_KEYMAP_ARG := $(if $(KEYMAP),--keymap "$(KEYMAP)",)
SIM_CONF_ARG := $(if $(CONF),--conf "$(CONF)",)
SIM_WEST_ARG := $(if $(WEST),--west "$(WEST)",)
SIM_ZMKREV_ARG := $(if $(ZMK_REV),--zmk-rev "$(ZMK_REV)",)
SIM_LVGL_MAJOR ?= 9
SIM_OFFLINE_ARG := $(if $(filter 1 y yes true,$(SIM_OFFLINE)),--offline,)$(if $(filter 1 y yes true,$(SIM_REFRESH)), --refresh,)
# when a local KEYMAP is given we don't need the alphred_kb clone for it, but we
# still use its west.yml (unless WEST=...) to pick the ZMK revision.

SIM_ZMK := $(SIM_CACHE)/src/zmk
SIM_ZEPHYR := $(SIM_CACHE)/src/zephyr
SIM_LVGL := $(SIM_CACHE)/src/lvgl

sim_common_flags = -pipe -std=gnu11 -g -O1 -fno-strict-aliasing -fno-common -fdiagnostics-color=always \
  -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable \
  -Wno-int-conversion -Wno-incompatible-pointer-types -Wno-pointer-sign \
  -Wno-address-of-packed-member -Wno-format -Wno-cpp -Wno-attributes -Wno-unused-label \
  -Wno-missing-braces -Wno-implicit-fallthrough \
  -D__ZEPHYR__=1 -DKERNEL -D_GNU_SOURCE -DLV_CONF_INCLUDE_SIMPLE=1 \
  '-DLV_CONF_PATH="lv_conf.h"' -include sim/shim/include/sim_printf_override.h
# (Zephyr's modules/lvgl/CMakeLists.txt defines exactly these two for every
# source; LV_CONF_PATH is needed because LVGL's Kconfig turns on LV_CONF_SKIP.
# "lv_conf.h" resolves to zephyr/modules/lvgl/include/lv_conf.h via -I.)
# sim_printf_override.h: LVGL's builtin printf defines the firmware's
# snprintf/vsnprintf (as on hardware); see the header for why it is renamed.

# $(1)=role $(2)=link order $(3)=extra flags
sim_cc = @echo "  CC [$(1)] $<" && $(SIM_CC) $(sim_common_flags) $(3) -DSIM_LINK_ORDER=$(2) \
  -imacros $(SIM_GEN)/$(1)/include/generated/zephyr/autoconf.h \
  -I$(SIM_DIR)/shim/include -I$(SIM_GEN)/$(1)/include -I$(SIM_GEN)/$(1)/include/generated \
  -I$(SIM_GEN) \
  -I$(SIM_ZMK)/app/include -I$(SIM_ZMK)/app/module/include -I$(SIM_ZEPHYR)/include -I$(SIM_ZEPHYR)/modules/lvgl/include \
  -I$(SIM_LVGL) -I$(SIM_LVGL)/src -I$(dir $<) \
  -MMD -MP -c $< -o $@

.PHONY: sim sim-build sim-run sim-watch sim-test sim-deps sim-clean sim-distclean sim-fetch sim-gen sim-compile

SIM_PANEL_ARG = $(if $(PANEL_EMU),--panel $(PANEL_EMU),)$(if $(EPAPER_EMU), --epaper $(EPAPER_EMU),)

# A freshly linked sim.exe can be briefly locked (e.g. by an antivirus scan:
# "Device or resource busy"); wait until it starts before the real run.
SIM_WAIT_EXE = @for i in 1 2 3 4 5 6 7 8 9 10; do $(SIM_BUILD)/bin/sim.exe --help >/dev/null 2>&1 && break; sleep 1; done

sim: sim-build
	$(SIM_WAIT_EXE)
	@echo "=== Starting simulator (PANEL_EMU=$(if $(PANEL_EMU),$(PANEL_EMU),ideal)) ==="
	$(SIM_BUILD)/bin/sim.exe $(SIM_PANEL_ARG) $(SIM_ARGS)

sim-run:
	$(SIM_BUILD)/bin/sim.exe $(SIM_PANEL_ARG) $(SIM_ARGS)

# edit -> see loop: the window stays open; sim/tools/watch.py rebuilds on save
# (offline, incremental) and sim.exe --watch cold-boots the rebuilt half
SIM_WATCH_ART ?= auto
SIM_PASS_VARS = $(foreach v,KEYMAP CONF WEST ZMK_REV SIM_LVGL_MAJOR SIM_MSYS2,$(if $($(v)),--build-arg "$(v)=$($(v))",))
sim-watch: sim-build
	$(SIM_WAIT_EXE)
	@$(SIM_PY) $(SIM_DIR)/tools/watch.py --make "$(SIM_MSYS2)/usr/bin/make.exe" \
	  --art-target "$(SIM_WATCH_ART)" $(SIM_PASS_VARS) \
	  -- $(SIM_BUILD)/bin/sim.exe --watch $(SIM_PANEL_ARG) $(SIM_ARGS)

# headless smoke test: scripted input, expectations, PNG + GIF output
#   then the central Beedly scenario (sim/examples/beedly.txt) and the typing-only WPM
#   counter (sim/examples/wpm.txt): every frame is dumped and
#   sim/tools/check_beedly.py compares the Beedly slot with the expected Aseprite tag (the
#   frame check needs the pixel-exact ideal panel, so it always runs with --panel ideal)
SIM_BEEDLY_OUT := $(SIM_BUILD)/out/beedly
sim-test: sim-build
	$(SIM_WAIT_EXE)
	$(SIM_BUILD)/bin/sim.exe --headless --script $(SIM_DIR)/examples/smoke.txt $(SIM_PANEL_ARG) --png $(SIM_BUILD)/out/smoke.png --gif $(SIM_BUILD)/out/smoke.gif $(SIM_ARGS)
	@rm -rf $(SIM_BEEDLY_OUT)/frames && mkdir -p $(SIM_BEEDLY_OUT)/frames
	$(SIM_BUILD)/bin/sim.exe --headless --script $(SIM_DIR)/examples/beedly.txt --panel ideal --frames $(SIM_BEEDLY_OUT)/frames --png $(SIM_BEEDLY_OUT)/final.png --gif $(SIM_BEEDLY_OUT)/beedly.gif
	$(SIM_PY) $(SIM_DIR)/tools/check_beedly.py --frames $(SIM_BEEDLY_OUT)/frames --script $(SIM_DIR)/examples/beedly.txt
	$(SIM_BUILD)/bin/sim.exe --headless --script $(SIM_DIR)/examples/wpm.txt --png $(SIM_BUILD)/out/wpm.png

# parallel compile by default (NUMBER_OF_PROCESSORS is set by Windows); an
# explicit make -jN wins
SIM_JOBS ?= $(if $(NUMBER_OF_PROCESSORS),$(NUMBER_OF_PROCESSORS),4)
SIM_JOBS_ARG = $(if $(filter -j%,$(MAKEFLAGS)) $(findstring j,$(firstword -$(MAKEFLAGS))),,-j$(SIM_JOBS))

sim-build: sim-check-toolchain sim-gen
	@$(MAKE) --no-print-directory $(SIM_JOBS_ARG) -f $(SIM_DIR)/sim.mk sim-compile SIM_MSYS2="$(SIM_MSYS2)"

sim-check-toolchain:
	@$(SIM_PY) $(SIM_DIR)/tools/setup_toolchain.py --msys2 "$(SIM_MSYS2)" --env $(SIM_ENV) --check

sim-deps:
	@$(SIM_PY) $(SIM_DIR)/tools/setup_toolchain.py --msys2 "$(SIM_MSYS2)" --env $(SIM_ENV) --install

sim-fetch:
	@$(SIM_PY) $(SIM_DIR)/tools/fetch_deps.py --cache $(SIM_CACHE) $(SIM_WEST_ARG) $(SIM_ZMKREV_ARG) $(SIM_OFFLINE_ARG) --widget-lvgl $(SIM_LVGL_MAJOR)

sim-gen: sim-fetch
	@$(SIM_PY) $(SIM_DIR)/tools/gen_config.py --cache $(SIM_CACHE) \
	  --out $(SIM_GEN) --cc "$(SIM_CC)" $(SIM_KEYMAP_ARG) $(SIM_CONF_ARG)

sim-clean:
	rm -rf $(SIM_BUILD)

sim-distclean: sim-clean
	rm -rf $(SIM_CACHE)

# ---------------------------------------------------------------- compile
# (only evaluated in the sub-make, after sim-gen produced the source lists)

ifneq ($(filter sim-compile,$(MAKECMDGOALS)),)
export PATH := $(SIM_TOOLBIN_POSIX):$(PATH)
# gcc/collect2 need a writable temp dir (TMP may point at C:\Windows in some shells)
SIM_TMP := $(shell mkdir -p $(SIM_BUILD)/tmp && cygpath -w "$(CURDIR)/$(SIM_BUILD)/tmp" 2>/dev/null || echo "$(CURDIR)/$(SIM_BUILD)/tmp")
export TMP := $(SIM_TMP)
export TEMP := $(SIM_TMP)
export TMPDIR := $(SIM_TMP)

include $(SIM_GEN)/central/sources.mk
include $(SIM_GEN)/peripheral/sources.mk

SIM_BIN := $(SIM_BUILD)/bin
SIM_DLL_LDFLAGS := -shared -static-libgcc \
  -Wl,--wrap=sys_heap_init -Wl,--wrap=sys_heap_alloc -Wl,--wrap=sys_heap_aligned_realloc

sim-compile: $(SIM_BIN)/central.dll $(SIM_BIN)/peripheral.dll $(SIM_BIN)/sim.exe $(SIM_BIN)/SDL2.dll

%/.dir:
	@mkdir -p $(@D) && touch $@

$(SIM_BUILD)/central/liblvgl.a: $(SIM_CENTRAL_LVGL_OBJS)
	@echo "  AR  $@" && rm -f $@ && $(SIM_AR) rcs $@ $^
$(SIM_BUILD)/peripheral/liblvgl.a: $(SIM_PERIPHERAL_LVGL_OBJS)
	@echo "  AR  $@" && rm -f $@ && $(SIM_AR) rcs $@ $^

$(SIM_BIN)/central.dll: $(SIM_CENTRAL_OBJS) $(SIM_BUILD)/central/liblvgl.a | $(SIM_BIN)/.dir
	@echo "  LD  $@" && $(SIM_CC) $(SIM_DLL_LDFLAGS) -o $@ $(SIM_CENTRAL_OBJS) $(SIM_BUILD)/central/liblvgl.a -lm
$(SIM_BIN)/peripheral.dll: $(SIM_PERIPHERAL_OBJS) $(SIM_BUILD)/peripheral/liblvgl.a | $(SIM_BIN)/.dir
	@echo "  LD  $@" && $(SIM_CC) $(SIM_DLL_LDFLAGS) -o $@ $(SIM_PERIPHERAL_OBJS) $(SIM_BUILD)/peripheral/liblvgl.a -lm

SIM_HOST_SRCS := $(wildcard $(SIM_DIR)/host/*.c)
SIM_HOST_OBJS := $(patsubst $(SIM_DIR)/host/%.c,$(SIM_BUILD)/host/%.o,$(SIM_HOST_SRCS))
SIM_SDL_CFLAGS := -I$(SIM_MSYS2)/$(SIM_ENV)/include/SDL2 -Dmain=SDL_main
SIM_SDL_LIBS := -L$(SIM_MSYS2)/$(SIM_ENV)/lib -lmingw32 -lSDL2main -lSDL2 -lgdi32

$(SIM_BUILD)/host/%.o: $(SIM_DIR)/host/%.c | $(SIM_BUILD)/host/.dir
	@echo "  CC [host] $<" && $(SIM_CC) -pipe -std=gnu11 -g -O2 -Wall -Wno-unused-function $(SIM_SDL_CFLAGS) \
	  -I$(SIM_GEN) -MMD -MP -c $< -o $@

# link to a temporary name and rename: Windows refuses to overwrite a running
# exe but allows renaming it (sim-watch keeps sim.exe running across rebuilds)
$(SIM_BIN)/sim.exe: $(SIM_HOST_OBJS) | $(SIM_BIN)/.dir
	@echo "  LD  $@" && $(SIM_CC) -static-libgcc -o $@.new $^ $(SIM_SDL_LIBS) -lm && \
	  { rm -f $@.old; mv -f $@ $@.old 2>/dev/null; mv -f $@.new $@; rm -f $@.old 2>/dev/null; true; }

$(SIM_BIN)/SDL2.dll: | $(SIM_BIN)/.dir
	@cp "$(SIM_TOOLBIN)/SDL2.dll" $@

-include $(SIM_CENTRAL_OBJS:.o=.d) $(SIM_PERIPHERAL_OBJS:.o=.d) $(SIM_HOST_OBJS:.o=.d)
# LVGL objects: no .d files (stat-ing their ~60k header dependencies made a
# no-op build take ~8 s on Windows).  The LVGL/Zephyr sources are pinned by
# deps.json (rewritten only when a revision changes), and the configuration
# reaches LVGL through autoconf.h, so those two are their dependencies.
$(SIM_CENTRAL_LVGL_OBJS): $(SIM_GEN)/central/include/generated/zephyr/autoconf.h $(SIM_CACHE)/deps.json
$(SIM_PERIPHERAL_LVGL_OBJS): $(SIM_GEN)/peripheral/include/generated/zephyr/autoconf.h $(SIM_CACHE)/deps.json
endif
