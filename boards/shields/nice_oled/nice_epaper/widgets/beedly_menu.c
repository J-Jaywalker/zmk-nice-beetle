/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Beedly pet menu on the central (left) screen. See beedly_menu.h for the layout and the rules;
 * behaviors/behavior_beedly.c calls the state functions, screen.c draws it.
 */

#include <ctype.h>
#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <dt-bindings/zmk/beedly.h>
#include <zmk/display.h>
#include <zmk/keymap.h>

#include "beedly_menu.h"
#include "screen.h"
#include "../assets/custom_fonts.h"

/* Upright-screen geometry of the menu box (see beedly_menu.h) */
#define MENU_H (BEEDLY_MENU_BOTTOM - BEEDLY_MENU_TOP + 1) /* 54: frame + 2 + 3 x 16 + 2 + frame */
#define ROW_TOP(i) (BEEDLY_MENU_TOP + 3 + (i) * 16)        /* 67, 83, 99 */
#define ROW_H 15
#define ROW_LEFT 2
#define ROW_W (CANVAS_WIDTH - 2 * ROW_LEFT) /* 64 */
#define TEXT_LEFT 6

static atomic_t cursor = ATOMIC_INIT(BEEDLY_MENU_PET);

/*
 * The menu layer's id, looked up by name once found (layer ids are stable; indexes can be
 * reordered). Not cached while missing, so a keymap without the layer just logs and does nothing.
 */
static zmk_keymap_layer_id_t menu_layer = ZMK_KEYMAP_LAYER_ID_INVAL;

static bool name_matches(const char *name, const char *wanted) {
    if (name == NULL) {
        return false;
    }
    while (*name && *wanted) {
        if (toupper((unsigned char)*name++) != toupper((unsigned char)*wanted++)) {
            return false;
        }
    }
    return *name == '\0' && *wanted == '\0';
}

static zmk_keymap_layer_id_t find_menu_layer(void) {
    if (menu_layer != ZMK_KEYMAP_LAYER_ID_INVAL) {
        return menu_layer;
    }
    for (zmk_keymap_layer_index_t i = 0; i < ZMK_KEYMAP_LAYERS_LEN; i++) {
        zmk_keymap_layer_id_t id = zmk_keymap_layer_index_to_id(i);
        if (id != ZMK_KEYMAP_LAYER_ID_INVAL &&
            name_matches(zmk_keymap_layer_name(id), CONFIG_NICE_BEETLE_PET_LAYER_NAME)) {
            menu_layer = id;
            break;
        }
    }
    return menu_layer;
}

/*
 * Redraw request: behaviour callbacks run in the key/split context, so they only submit this
 * work; the redraw itself (canvas drawing) runs on the display work queue.
 */
static void menu_redraw_work_cb(struct k_work *work) { zmk_widget_screen_refresh(); }

static K_WORK_DEFINE(menu_redraw_work, menu_redraw_work_cb);

static void request_redraw(void) {
    if (zmk_display_is_initialized()) {
        k_work_submit_to_queue(zmk_display_work_q(), &menu_redraw_work);
    }
}

bool beedly_menu_is_open(void) {
    zmk_keymap_layer_id_t id = find_menu_layer();
    return id != ZMK_KEYMAP_LAYER_ID_INVAL && zmk_keymap_layer_active(id);
}

uint8_t beedly_menu_cursor(void) { return (uint8_t)atomic_get(&cursor); }

void beedly_menu_open(void) {
    zmk_keymap_layer_id_t id = find_menu_layer();
    if (id == ZMK_KEYMAP_LAYER_ID_INVAL) {
        LOG_WRN("beedly: no keymap layer named \"%s\" (NICE_BEETLE_PET_LAYER_NAME)",
                CONFIG_NICE_BEETLE_PET_LAYER_NAME);
        return;
    }
    bool was_open = zmk_keymap_layer_active(id);
    atomic_set(&cursor, BEEDLY_MENU_PET);
    // Locking, like &tog / &to: momentary layer releases do not close the menu. The layer
    // change redraws the screen (screen.c layer listener); if the layer was already active
    // there is no layer event, so ask for the redraw of the reset cursor here.
    zmk_keymap_layer_activate(id, true);
    if (was_open) {
        request_redraw();
    }
}

void beedly_menu_move(int delta) {
    if (!beedly_menu_is_open()) {
        return;
    }
    int next = CLAMP((int)atomic_get(&cursor) + delta, 0, BEEDLY_MENU_ITEM_COUNT - 1);
    if (atomic_set(&cursor, next) != next) {
        request_redraw();
    }
}

bool beedly_menu_selected_action(uint32_t *action) {
    if (!beedly_menu_is_open()) {
        return false;
    }
    switch (atomic_get(&cursor)) {
    case BEEDLY_MENU_PET:
        *action = BEEDLY_PET;
        return true;
    case BEEDLY_MENU_FEED:
        *action = BEEDLY_FEED;
        return true;
    case BEEDLY_MENU_SAVE:
        *action = BEEDLY_SAVE;
        return true;
    default:
        return false;
    }
}

void beedly_menu_close(void) {
    zmk_keymap_layer_id_t id = find_menu_layer();
    if (id != ZMK_KEYMAP_LAYER_ID_INVAL) {
        // Same locking flag as beedly_menu_open(); the layer below (ALPH-WIN / ALPH-MAC, or
        // whatever was active) shows again. The layer event redraws the screen without the menu.
        zmk_keymap_layer_deactivate(id, true);
    }
}

void draw_beedly_menu(lv_obj_t *canvas, uint8_t selected) {
    static const char *const labels[BEEDLY_MENU_ITEM_COUNT] = {"PET", "FEED", "SAVE"};

    lv_draw_rect_dsc_t rect_fg_dsc;
    init_rect_dsc(&rect_fg_dsc, LVGL_FOREGROUND);
    lv_draw_rect_dsc_t rect_bg_dsc;
    init_rect_dsc(&rect_bg_dsc, LVGL_BACKGROUND);
    lv_draw_label_dsc_t label_fg_dsc;
    init_label_dsc(&label_fg_dsc, LVGL_FOREGROUND, &pixel_operator_mono, LV_TEXT_ALIGN_LEFT);
    lv_draw_label_dsc_t label_bg_dsc;
    init_label_dsc(&label_bg_dsc, LVGL_BACKGROUND, &pixel_operator_mono, LV_TEXT_ALIGN_LEFT);

    // 1 px frame: a foreground box with a background box one pixel inside it
    canvas_draw_rect(canvas, 0, BEEDLY_MENU_TOP, CANVAS_WIDTH, MENU_H, &rect_fg_dsc);
    canvas_draw_rect(canvas, 1, BEEDLY_MENU_TOP + 1, CANVAS_WIDTH - 2, MENU_H - 2, &rect_bg_dsc);

    char text[8];
    for (int i = 0; i < BEEDLY_MENU_ITEM_COUNT; i++) {
        bool is_selected = (i == selected);
        snprintf(text, sizeof(text), "%c %s", is_selected ? '>' : ' ', labels[i]);
        if (is_selected) {
            canvas_draw_rect(canvas, ROW_LEFT, ROW_TOP(i), ROW_W, ROW_H, &rect_fg_dsc);
        }
        // 13 px font line, caps on its rows 2..10: centred in the 15 px row
        canvas_draw_text(canvas, TEXT_LEFT, ROW_TOP(i) + 1, CANVAS_WIDTH - TEXT_LEFT,
                         is_selected ? &label_bg_dsc : &label_fg_dsc, text);
    }
}
