/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Central-screen Beedly: one animation slot shared by luna.c, modifiers.c and hid_indicators.c.
 * See beedly_slot.h for the precedence rules.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <dt-bindings/zmk/modifiers.h>

#include "beedly_slot.h"

#define MODIFIER_FRAME_MS CONFIG_NICE_BEETLE_LUNA_MODIFIER_FRAME_MS
#define CAPSLOCK_FRAME_MS CONFIG_NICE_BEETLE_LUNA_CAPSLOCK_FRAME_MS

enum beedly_anim {
    BEEDLY_ANIM_NONE,
    BEEDLY_ANIM_IDLE,
    BEEDLY_ANIM_WALK,
    BEEDLY_ANIM_SPRINT,
    BEEDLY_ANIM_SHIFT,
    BEEDLY_ANIM_CTRL,
    BEEDLY_ANIM_ALT,
    BEEDLY_ANIM_CAPSLOCK,
};

struct beedly_anim_def {
    const lv_image_dsc_t *const *frames;
    const size_t *count; /* generated, so not a compile-time constant */
    uint32_t frame_ms;
};

static const struct beedly_anim_def anim_defs[] = {
    [BEEDLY_ANIM_IDLE] = {beedly_luna_idle, &beedly_luna_idle_count,
                          CONFIG_NICE_BEETLE_LUNA_IDLE_FRAME_MS},
    [BEEDLY_ANIM_WALK] = {beedly_luna_walk, &beedly_luna_walk_count,
                          CONFIG_NICE_BEETLE_LUNA_WALK_FRAME_MS},
    [BEEDLY_ANIM_SPRINT] = {beedly_luna_sprint, &beedly_luna_sprint_count,
                            CONFIG_NICE_BEETLE_LUNA_SPRINT_FRAME_MS},
    [BEEDLY_ANIM_SHIFT] = {beedly_luna_shift, &beedly_luna_shift_count, MODIFIER_FRAME_MS},
    [BEEDLY_ANIM_CTRL] = {beedly_luna_ctrl, &beedly_luna_ctrl_count, MODIFIER_FRAME_MS},
    [BEEDLY_ANIM_ALT] = {beedly_luna_alt, &beedly_luna_alt_count, MODIFIER_FRAME_MS},
    [BEEDLY_ANIM_CAPSLOCK] = {beedly_luna_capslock, &beedly_luna_capslock_count,
                              CAPSLOCK_FRAME_MS},
};

static lv_obj_t *slot_obj;
static enum beedly_anim current = BEEDLY_ANIM_NONE;
static enum beedly_anim wpm_anim = BEEDLY_ANIM_NONE; /* NONE until luna.c reports a WPM */
static uint8_t mods;
static bool caps_lock;

static enum beedly_anim choose(void) {
    if (mods & (MOD_LGUI | MOD_RGUI)) {
        return BEEDLY_ANIM_IDLE;
    }
    if (mods & (MOD_LALT | MOD_RALT)) {
        return BEEDLY_ANIM_ALT;
    }
    if (mods & (MOD_LCTL | MOD_RCTL)) {
        return BEEDLY_ANIM_CTRL;
    }
    if (mods & (MOD_LSFT | MOD_RSFT)) {
        return BEEDLY_ANIM_SHIFT;
    }
    if (caps_lock) {
        return BEEDLY_ANIM_CAPSLOCK;
    }
    return wpm_anim;
}

static void apply(void) {
    if (slot_obj == NULL) {
        return;
    }
    enum beedly_anim next = choose();
    if (next == current) {
        return; /* keep the running animation: no restart, no flicker */
    }
    LOG_DBG("beedly: mods 0x%02x caps %d wpm anim %d -> anim %d (was %d)", mods, caps_lock,
            wpm_anim, next, current);
    current = next;

    if (next == BEEDLY_ANIM_NONE) {
        lv_anim_delete(slot_obj, NULL);
        lv_obj_add_flag(slot_obj, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    const struct beedly_anim_def *def = &anim_defs[next];
    const size_t count = *def->count;
    /* lv_animimg_start() replaces the running animation of this object (LVGL removes concurrent
     * animations with the same exec callback) and shows frame 0 right away (early apply). */
    lv_animimg_set_src(slot_obj, (const void **)def->frames, count);
    lv_animimg_set_duration(slot_obj, def->frame_ms * count);
    lv_animimg_set_repeat_count(slot_obj, LV_ANIM_REPEAT_INFINITE);
    lv_animimg_start(slot_obj);
    lv_obj_remove_flag(slot_obj, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *beedly_slot_init(lv_obj_t *parent) {
    if (slot_obj == NULL) {
        slot_obj = lv_animimg_create(parent);
        lv_obj_set_pos(slot_obj, BEEDLY_SLOT_PANEL_X, BEEDLY_SLOT_PANEL_Y);
        lv_obj_add_flag(slot_obj, LV_OBJ_FLAG_HIDDEN);
        current = BEEDLY_ANIM_NONE;
        apply();
    }
    return slot_obj;
}

lv_obj_t *beedly_slot_obj(void) { return slot_obj; }

void beedly_slot_set_wpm(uint8_t wpm) {
    if (wpm < CONFIG_NICE_BEETLE_LUNA_WALK_WPM) {
        wpm_anim = BEEDLY_ANIM_IDLE;
    } else if (wpm < CONFIG_NICE_BEETLE_LUNA_SPRINT_WPM) {
        wpm_anim = BEEDLY_ANIM_WALK;
    } else {
        wpm_anim = BEEDLY_ANIM_SPRINT;
    }
    apply();
}

void beedly_slot_set_mods(uint8_t new_mods) {
    mods = new_mods;
    apply();
}

void beedly_slot_set_caps_lock(bool on) {
    caps_lock = on;
    apply();
}
