/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <lvgl.h>

/*
 * Beedly pet menu, CENTRAL (left half) only. The central owns nothing of the pet but a cursor:
 * PET, FEED, SAVE (top to bottom). The pet itself lives on the peripheral (beedly_pet.c).
 *
 * The menu is "open" while the keymap layer named CONFIG_NICE_BEETLE_PET_LAYER_NAME is active,
 * so leaving that layer by any other key (e.g. a `&to` on it) closes it too. It is drawn by
 * screen.c's draw_canvas() as the last layer of the normal left screen, over the WPM graph and
 * the WPM label (rows BEEDLY_MENU_TOP..BEEDLY_MENU_BOTTOM of the upright 68 x 160 screen):
 *
 *     0..15   SIG + output icon          (unchanged)
 *    19..29   BAT text                   (unchanged)
 *    31..62   Beedly slot (beedly_slot.h, unchanged, keeps animating)
 *    64..117  MENU: 1 px frame, three 15 px rows PET / FEED / SAVE, cursor = inverted row + ">"
 *             (covers the WPM grid/graph at 65..97 and the WPM label at 103..115)
 *   129..131  BLE profile dots           (unchanged)
 *   146..158  layer label, reads "BEEDLY" while the menu is open (acts as the menu title)
 *
 * Drawn with the canvas helpers of util.c into the existing canvas: no LVGL object, nothing
 * allocated from the LVGL pool while the menu is closed or open (only the transient draw tasks
 * of each draw call, like every other widget), and a single `if` per redraw when closed.
 */
#define BEEDLY_MENU_TOP 64
#define BEEDLY_MENU_BOTTOM 117

enum beedly_menu_item {
    BEEDLY_MENU_PET,
    BEEDLY_MENU_FEED,
    BEEDLY_MENU_SAVE,
    BEEDLY_MENU_ITEM_COUNT,
};

/*
 * Behaviour side (key / split context): only plain state changes and a k_work submit to the
 * display work queue; no LVGL calls.
 */
void beedly_menu_open(void);
void beedly_menu_move(int delta);
/* The remote action (BEEDLY_PET/FEED/SAVE) for the cursor; false if the menu is not open. */
bool beedly_menu_selected_action(uint32_t *action);
/* Deactivate the menu layer (SAVE), revealing the layer below it. */
void beedly_menu_close(void);

/* State readers (any context). */
bool beedly_menu_is_open(void);
uint8_t beedly_menu_cursor(void);

/* Display thread: called last by screen.c's draw_canvas() when the menu is open. */
void draw_beedly_menu(lv_obj_t *canvas, uint8_t cursor);
