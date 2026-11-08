/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <lvgl.h>

#include "../assets/beedly_luna_frames.h"
#include "util.h"

/*
 * The central screen's Beedly (it replaces the Luna dog). ONE lv_animimg object, owned here, shows
 * one animation at a time; luna.c (WPM), modifiers.c (held modifiers) and hid_indicators.c
 * (Caps Lock) only report their state and this module picks the animation:
 *
 *   1. Gui held    -> Idle      (no dedicated Gui animation; Gui used to show Luna sitting)
 *   2. Alt held    -> Alt
 *   3. Ctrl held   -> Ctrl
 *   4. Shift held  -> Shift      (1-4: the old modifiers.c priority, first match wins)
 *   5. Caps Lock   -> CAPSLOCK
 *   6. otherwise   -> Idle / Walk / Sprint from the WPM (hidden if the WPM widget is disabled)
 *
 * The object is only reconfigured when the chosen animation changes, so state updates that keep
 * the same animation do not restart or flicker it, and nothing is created or deleted at run time.
 */

/*
 * Slot position in the UPRIGHT screen (portrait, 68 px wide x 160 px tall, as you look at the
 * nice!view): top-left corner of the 32 x 32 sprite. Measured in the simulator: the "BAT" text
 * ends on row 29 and the WPM graph line reaches up to row 64 at the fixed-range maximum, so rows
 * 30..63 (34 px) are free; top 31 centres the sprite with one empty row on each side, and
 * left (68 - 32) / 2 = 18 centres it horizontally. The old Luna slot was panel (100, 15).
 * sim/tools/check_beedly.py reads these two defines.
 */
#define BEEDLY_SLOT_TOP 31
#define BEEDLY_SLOT_LEFT ((CANVAS_WIDTH - BEEDLY_LUNA_FRAME_H) / 2)

/* The same corner in the panel's landscape coordinates (the frames are stored rotated). */
#define BEEDLY_SLOT_PANEL_X (CANVAS_HEIGHT - BEEDLY_SLOT_TOP - BEEDLY_LUNA_FRAME_W)
#define BEEDLY_SLOT_PANEL_Y BEEDLY_SLOT_LEFT

/* Create the object (once; later calls return the existing one). */
lv_obj_t *beedly_slot_init(lv_obj_t *parent);
lv_obj_t *beedly_slot_obj(void);

/* State inputs (call from the display thread, i.e. widget update callbacks). */
void beedly_slot_set_wpm(uint8_t wpm);
void beedly_slot_set_mods(uint8_t mods);
void beedly_slot_set_caps_lock(bool on);
