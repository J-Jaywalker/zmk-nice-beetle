/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Beedly pet on the peripheral (right) half: stats, PET / FEED animation, persistence and the
 * overlays drawn over the right screen's Beedly art. See beedly_pet.h for the model and layout.
 *
 * Threads:
 *   - behaviour (split command) context: beedly_pet_request() only stores the action and
 *     submits work, never touches LVGL;
 *   - display work queue: applies PET / FEED, runs the animation (one-shot delayable work,
 *     armed only after an effective action, stops itself) and draws;
 *   - system work queue: the one-shot save, the activity listener (IDLE / SLEEP).
 * The model is shared by all three and protected by a spinlock; settings_save_one() is never
 * called with it held.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <dt-bindings/zmk/beedly.h>
#include <zmk/activity.h>
#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>

#include "beedly_pet.h"
#include "util.h"

#define HEARTS_STEP CONFIG_NICE_BEETLE_PET_HEARTS_STEP
#define FOOD_STEP CONFIG_NICE_BEETLE_PET_FOOD_STEP
#define XP_PER_ACTION CONFIG_NICE_BEETLE_PET_XP_PER_ACTION
#define XP_PER_LEVEL CONFIG_NICE_BEETLE_PET_XP_PER_LEVEL
#define MAX_LEVEL CONFIG_NICE_BEETLE_PET_MAX_LEVEL
#define ANIM_STEP_MS CONFIG_NICE_BEETLE_PET_ANIM_STEP_MS
#define ANIM_STEPS 4 /* bar: old, new, old, new; hearts float up; then the final state */

/* L8 values of the screen's colours (LVGL_FOREGROUND / LVGL_BACKGROUND in util.h) */
#define INK (IS_ENABLED(CONFIG_NICE_VIEW_WIDGET_INVERTED) ? 0xFF : 0x00)
#define PAPER (IS_ENABLED(CONFIG_NICE_VIEW_WIDGET_INVERTED) ? 0x00 : 0xFF)

/**
 * Model
 **/

struct pet_model {
    uint8_t hearts;
    uint8_t food;
    uint8_t level;
    uint16_t xp;
    /* uptime (ms) the stored hearts / food value is valid for; the remainder of a decay
     * period carries over, so frequent redraws do not slow the decay down */
    int64_t hearts_anchor;
    int64_t food_anchor;
    bool dirty; /* changed by an action since the last save */
};

static struct pet_model pet = {
    .hearts = BEEDLY_PET_BAR_MAX / 2,
    .food = BEEDLY_PET_BAR_MAX / 2,
    .level = 1,
};
static struct k_spinlock pet_lock;

static void decay(uint8_t *value, int64_t *anchor, int minutes, int64_t now) {
    if (minutes <= 0) {
        *anchor = now;
        return;
    }
    const int64_t period = (int64_t)minutes * 60 * 1000;
    const int64_t lost = (now - *anchor) / period;
    if (lost <= 0) {
        return;
    }
    *anchor += lost * period;
    *value = (lost >= *value) ? 0 : (uint8_t)(*value - lost);
}

/* Fold the decay up to now into the stored values (does not make the model dirty). */
static void normalize_locked(int64_t now) {
    decay(&pet.hearts, &pet.hearts_anchor, CONFIG_NICE_BEETLE_PET_HEARTS_DECAY_MINUTES, now);
    decay(&pet.food, &pet.food_anchor, CONFIG_NICE_BEETLE_PET_FOOD_DECAY_MINUTES, now);
}

static struct pet_model snapshot(void) {
    k_spinlock_key_t key = k_spin_lock(&pet_lock);
    normalize_locked(k_uptime_get());
    struct pet_model copy = pet;
    k_spin_unlock(&pet_lock, key);
    return copy;
}

static void award_xp_locked(void) {
    if (pet.level < MAX_LEVEL) {
        pet.xp += XP_PER_ACTION;
        while (pet.xp >= XP_PER_LEVEL && pet.level < MAX_LEVEL) {
            pet.xp -= XP_PER_LEVEL;
            pet.level++;
            LOG_INF("beedly: level up -> %d", pet.level);
        }
    }
    if (pet.level >= MAX_LEVEL) {
        pet.xp = 0; /* the XP bar shows full at the top level */
    }
}

/* PET / FEED. false = that bar is already full: nothing changes at all. */
static bool apply_action(uint32_t action, uint8_t *from, uint8_t *to) {
    bool effective = false;
    k_spinlock_key_t key = k_spin_lock(&pet_lock);
    normalize_locked(k_uptime_get());

    uint8_t *value = (action == BEEDLY_PET) ? &pet.hearts : &pet.food;
    const int step = (action == BEEDLY_PET) ? HEARTS_STEP : FOOD_STEP;
    if (*value < BEEDLY_PET_BAR_MAX) {
        *from = *value;
        *value = MIN(*value + step, BEEDLY_PET_BAR_MAX);
        *to = *value;
        award_xp_locked();
        pet.dirty = true;
        effective = true;
    }

    k_spin_unlock(&pet_lock, key);
    return effective;
}

/**
 * Persistence: one record, settings key "beedly/pet"
 **/

#define SETTINGS_KEY "beedly/pet"
#define RECORD_VERSION 1

struct pet_record {
    uint8_t version;
    uint8_t hearts;
    uint8_t food;
    uint8_t level;
    uint16_t xp;
} __packed;

static void pet_save(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
    k_spinlock_key_t key = k_spin_lock(&pet_lock);
    if (!pet.dirty) {
        k_spin_unlock(&pet_lock, key);
        return;
    }
    normalize_locked(k_uptime_get());
    const struct pet_record record = {
        .version = RECORD_VERSION,
        .hearts = pet.hearts,
        .food = pet.food,
        .level = pet.level,
        .xp = pet.xp,
    };
    pet.dirty = false;
    k_spin_unlock(&pet_lock, key);

    int err = settings_save_one(SETTINGS_KEY, &record, sizeof(record));
    if (err < 0) {
        LOG_WRN("beedly: saving %s failed (%d)", SETTINGS_KEY, err);
        key = k_spin_lock(&pet_lock);
        pet.dirty = true;
        k_spin_unlock(&pet_lock, key);
        return;
    }
    LOG_DBG("beedly: saved hearts %d food %d level %d xp %d", record.hearts, record.food,
            record.level, record.xp);
#endif
}

/* System work queue: the one-shot delayed save, SAVE from the menu and going idle. */
static void pet_save_work_cb(struct k_work *work) { pet_save(); }

static K_WORK_DELAYABLE_DEFINE(pet_save_work, pet_save_work_cb);

#if IS_ENABLED(CONFIG_SETTINGS)
static int pet_settings_set(const char *name, size_t len, settings_read_cb read_cb,
                            void *cb_arg) {
    const char *next;
    if (!settings_name_steq(name, "pet", &next) || next != NULL) {
        return -ENOENT;
    }

    struct pet_record record;
    if (len != sizeof(record)) {
        LOG_WRN("beedly: %s has %d bytes, expected %d: using defaults", SETTINGS_KEY, (int)len,
                (int)sizeof(record));
        return 0;
    }
    int rc = read_cb(cb_arg, &record, sizeof(record));
    if (rc < 0) {
        return rc;
    }
    if (rc != sizeof(record) || record.version != RECORD_VERSION) {
        LOG_WRN("beedly: %s version %d, expected %d: using defaults", SETTINGS_KEY,
                record.version, RECORD_VERSION);
        return 0;
    }

    k_spinlock_key_t key = k_spin_lock(&pet_lock);
    pet.hearts = MIN(record.hearts, BEEDLY_PET_BAR_MAX);
    pet.food = MIN(record.food, BEEDLY_PET_BAR_MAX);
    pet.level = CLAMP(record.level, 1, MAX_LEVEL);
    pet.xp = (pet.level >= MAX_LEVEL) ? 0 : MIN(record.xp, XP_PER_LEVEL - 1);
    pet.hearts_anchor = pet.food_anchor = k_uptime_get();
    pet.dirty = false;
    k_spin_unlock(&pet_lock, key);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(beedly_pet_settings, "beedly", NULL, pet_settings_set, NULL, NULL);
#endif

/*
 * Going idle: save in the background if anything is unsaved. Going to sleep: ZMK raises SLEEP
 * synchronously right before sys_poweroff(), so save right here, synchronously (a deferred or
 * debounced save would never run).
 */
static int pet_activity_listener(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (ev == NULL || !beedly_pet_dirty()) {
        return ZMK_EV_EVENT_BUBBLE;
    }
    switch (ev->state) {
    case ZMK_ACTIVITY_IDLE:
        k_work_reschedule(&pet_save_work, K_NO_WAIT);
        break;
    case ZMK_ACTIVITY_SLEEP:
        k_work_cancel_delayable(&pet_save_work);
        pet_save();
        break;
    default:
        break;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(beedly_pet_activity, pet_activity_listener);
ZMK_SUBSCRIPTION(beedly_pet_activity, zmk_activity_state_changed);

/**
 * Overlays: small L8 canvases over the art, pixels written directly (upright coordinates)
 **/

struct overlay {
    lv_obj_t *canvas;
    uint8_t *buf;
    int16_t x0, y0, w, h; /* upright rectangle */
};

/* The buffer holds the panel (landscape) orientation: upright w x h is h wide, w tall. */
#define OVERLAY_BUF_SIZE(w, h) LV_CANVAS_BUF_SIZE(h, w, 8, LV_DRAW_BUF_STRIDE_ALIGN)

#define LEVEL_X 53
#define LEVEL_Y 125
#define LEVEL_W 8
#define LEVEL_H 5
#define BARS_X 0
#define BARS_Y 145
#define BARS_W CANVAS_WIDTH
#define BARS_H 7
#define HEARTS_X 58
#define HEARTS_Y 40
#define HEARTS_W 9
#define HEARTS_H 24

static uint8_t level_buf[OVERLAY_BUF_SIZE(LEVEL_W, LEVEL_H)] __aligned(LV_DRAW_BUF_ALIGN);
static uint8_t bars_buf[OVERLAY_BUF_SIZE(BARS_W, BARS_H)] __aligned(LV_DRAW_BUF_ALIGN);
static uint8_t hearts_buf[OVERLAY_BUF_SIZE(HEARTS_W, HEARTS_H)] __aligned(LV_DRAW_BUF_ALIGN);

#define OVERLAY(_buf, _x, _y, _w, _h) {.buf = (_buf), .x0 = (_x), .y0 = (_y), .w = (_w), .h = (_h)}
static struct overlay level_ov = OVERLAY(level_buf, LEVEL_X, LEVEL_Y, LEVEL_W, LEVEL_H);
static struct overlay bars_ov = OVERLAY(bars_buf, BARS_X, BARS_Y, BARS_W, BARS_H);
static struct overlay hearts_ov = OVERLAY(hearts_buf, HEARTS_X, HEARTS_Y, HEARTS_W, HEARTS_H);

static void overlay_create(struct overlay *ov, lv_obj_t *parent) {
    ov->canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(ov->canvas, ov->buf, ov->h, ov->w, LV_COLOR_FORMAT_L8);
    // upright (x, y) is panel (CANVAS_HEIGHT - 1 - y, x), as rotate_canvas() in util.c maps it
    lv_obj_set_pos(ov->canvas, CANVAS_HEIGHT - ov->y0 - ov->h, ov->x0);
}

static void overlay_fill(const struct overlay *ov, uint8_t value) {
    lv_draw_buf_t *db = lv_canvas_get_draw_buf(ov->canvas);
    for (int row = 0; row < ov->w; row++) {
        memset(db->data + row * db->header.stride, value, ov->h);
    }
}

/* x, y relative to the overlay's upright top-left corner */
static void overlay_px(const struct overlay *ov, int x, int y, uint8_t value) {
    if (x < 0 || y < 0 || x >= ov->w || y >= ov->h) {
        return;
    }
    lv_draw_buf_t *db = lv_canvas_get_draw_buf(ov->canvas);
    db->data[x * db->header.stride + (ov->h - 1 - y)] = value;
}

/* 1-bit bitmap, rows top to bottom, MSB = leftmost of `w` pixels; set bits get `value` */
static void overlay_bits(const struct overlay *ov, int x, int y, const uint8_t *rows, int w,
                         int h, uint8_t value) {
    for (int r = 0; r < h; r++) {
        for (int c = 0; c < w; c++) {
            if (rows[r] & BIT(w - 1 - c)) {
                overlay_px(ov, x + c, y + r, value);
            }
        }
    }
}

/* 3 x 5 digits in the style of the art's pill font (its "5" is digit 5 here) */
static const uint8_t digits[10][5] = {
    {07, 05, 05, 05, 07}, {02, 06, 02, 02, 07}, {07, 01, 07, 04, 07}, {07, 01, 07, 01, 07},
    {05, 05, 07, 01, 01}, {07, 04, 07, 01, 07}, {07, 04, 07, 05, 07}, {07, 01, 01, 01, 01},
    {07, 05, 07, 05, 07}, {07, 05, 07, 01, 07},
};

/* 7 x 6 heart */
static const uint8_t heart[6] = {0x36, 0x7f, 0x7f, 0x3e, 0x1c, 0x08};

static void draw_level(uint8_t level) {
    overlay_fill(&level_ov, INK); /* the pill is inked; the art's digits are paper on ink */
    // ones digit where the art's "5" is (x 58..60), tens at x 54..56
    overlay_bits(&level_ov, 5, 0, digits[level % 10], 3, 5, PAPER);
    if (level >= 10) {
        overlay_bits(&level_ov, 1, 0, digits[(level / 10) % 10], 3, 5, PAPER);
    }
}

/* The art's bar style: 20 x 7, rounded 1 px frame, 18 x 5 interior filled from the left. */
static void draw_bar(int left, int value, int max) {
    for (int x = left + 1; x <= left + 18; x++) {
        overlay_px(&bars_ov, x, 0, INK);
        overlay_px(&bars_ov, x, BARS_H - 1, INK);
    }
    int fill = (max > 0) ? value * 18 / max : 0;
    if (value > 0 && fill == 0) {
        fill = 1; /* anything above empty shows */
    }
    for (int y = 1; y < BARS_H - 1; y++) {
        overlay_px(&bars_ov, left, y, INK);
        overlay_px(&bars_ov, left + 19, y, INK);
        for (int x = left + 1; x <= left + fill; x++) {
            overlay_px(&bars_ov, x, y, INK);
        }
    }
}

/**
 * Animation (display work queue only)
 **/

enum pet_anim {
    PET_ANIM_NONE,
    PET_ANIM_HEARTS,
    PET_ANIM_FOOD,
};

static enum pet_anim anim = PET_ANIM_NONE;
static uint8_t anim_step;
static uint8_t anim_from; /* value of the changed bar before the action */

static void draw_hearts(void) {
    if (anim != PET_ANIM_HEARTS) {
        lv_obj_add_flag(hearts_ov.canvas, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    // three slots, bottom to top; the hearts float up: {0}, {0, 1}, {1, 2}, {2}
    static const uint8_t slots[ANIM_STEPS] = {0x1, 0x3, 0x6, 0x4};
    overlay_fill(&hearts_ov, PAPER);
    for (int slot = 0; slot < 3; slot++) {
        if (slots[anim_step] & BIT(slot)) {
            overlay_bits(&hearts_ov, 1, HEARTS_H - 7 - slot * 8, heart, 7, 6, INK);
        }
    }
    lv_obj_invalidate(hearts_ov.canvas);
    lv_obj_remove_flag(hearts_ov.canvas, LV_OBJ_FLAG_HIDDEN);
}

void beedly_pet_render(void) {
    if (bars_ov.canvas == NULL) {
        return; /* screen not created yet */
    }
    const struct pet_model now = snapshot();

    uint8_t hearts = now.hearts;
    uint8_t food = now.food;
    if (anim != PET_ANIM_NONE && (anim_step % 2) == 0) {
        // even steps show the changed bar as it was, odd ones the new value: it flashes up
        if (anim == PET_ANIM_HEARTS) {
            hearts = anim_from;
        } else {
            food = anim_from;
        }
    }

    draw_level(now.level);
    lv_obj_invalidate(level_ov.canvas);

    overlay_fill(&bars_ov, PAPER);
    draw_bar(2, hearts, BEEDLY_PET_BAR_MAX);
    draw_bar(24, food, BEEDLY_PET_BAR_MAX);
    if (now.level >= MAX_LEVEL) {
        draw_bar(46, 1, 1);
    } else {
        draw_bar(46, now.xp, XP_PER_LEVEL);
    }
    lv_obj_invalidate(bars_ov.canvas);

    draw_hearts();
}

static void pet_anim_work_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(pet_anim_work, pet_anim_work_cb);

static void pet_anim_work_cb(struct k_work *work) {
    if (++anim_step >= ANIM_STEPS) {
        anim = PET_ANIM_NONE; /* done: nothing re-armed */
    } else {
        k_work_schedule_for_queue(zmk_display_work_q(), &pet_anim_work, K_MSEC(ANIM_STEP_MS));
    }
    beedly_pet_render();
}

/* Action from the behaviour; 0 = none (BEEDLY_OPEN never reaches the peripheral) */
static atomic_t pending_action = ATOMIC_INIT(0);

static void pet_action_work_cb(struct k_work *work) {
    const uint32_t action = (uint32_t)atomic_set(&pending_action, 0);
    if (action != BEEDLY_PET && action != BEEDLY_FEED) {
        return;
    }
    if (anim != PET_ANIM_NONE) {
        LOG_DBG("beedly: action %d ignored, animation running", action);
        return;
    }
    uint8_t from, to;
    if (!apply_action(action, &from, &to)) {
        LOG_DBG("beedly: action %d ignored, bar full", action);
        return;
    }
    LOG_DBG("beedly: action %d %d -> %d", action, from, to);

    anim = (action == BEEDLY_PET) ? PET_ANIM_HEARTS : PET_ANIM_FOOD;
    anim_step = 0;
    anim_from = from;
    k_work_schedule_for_queue(zmk_display_work_q(), &pet_anim_work, K_MSEC(ANIM_STEP_MS));
    // one-shot save: armed by the first effective action, not pushed back by later ones
    k_work_schedule(&pet_save_work, K_SECONDS(CONFIG_NICE_BEETLE_PET_SAVE_DELAY_SECONDS));
    beedly_pet_render();
}

static K_WORK_DEFINE(pet_action_work, pet_action_work_cb);

/**
 * Public API
 **/

int beedly_pet_request(uint32_t action) {
    switch (action) {
    case BEEDLY_PET:
    case BEEDLY_FEED:
        atomic_set(&pending_action, action);
        k_work_submit_to_queue(zmk_display_work_q(), &pet_action_work);
        return 0;
    case BEEDLY_SAVE:
        k_work_reschedule(&pet_save_work, K_NO_WAIT);
        return 0;
    default:
        return -ENOTSUP;
    }
}

void beedly_pet_init(lv_obj_t *parent) {
    if (bars_ov.canvas != NULL) {
        return;
    }
    // created after the art (draw_animation()), so drawn on top of it
    overlay_create(&level_ov, parent);
    overlay_create(&bars_ov, parent);
    overlay_create(&hearts_ov, parent);
    lv_obj_add_flag(hearts_ov.canvas, LV_OBJ_FLAG_HIDDEN);
    beedly_pet_render();
}

uint8_t beedly_pet_hearts(void) { return snapshot().hearts; }
uint8_t beedly_pet_food(void) { return snapshot().food; }
uint8_t beedly_pet_level(void) { return snapshot().level; }
uint16_t beedly_pet_xp(void) { return snapshot().xp; }
bool beedly_pet_dirty(void) { return snapshot().dirty; }
bool beedly_pet_animating(void) { return anim != PET_ANIM_NONE; }
