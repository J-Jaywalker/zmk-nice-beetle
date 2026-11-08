/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * WPM-driven Beedly on the central screen (the widget is still called "luna": it replaced the
 * Luna dog). Below CONFIG_NICE_BEETLE_LUNA_WALK_WPM the Idle tag plays, up to
 * CONFIG_NICE_BEETLE_LUNA_SPRINT_WPM the Walk tag, above that Sprint. The frames come from the
 * WPM comes from wpm_counter.c (typing keys only, see wpm_counter.h). The frames come from the
 * generated assets/beedly_luna_frames.c (`make beedly_left`); the shared slot in beedly_slot.c
 * decides whether a modifier / Caps Lock animation is shown instead.
 */

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/display.h>
#include <zmk/event_manager.h>


#include "beedly_slot.h"
#include "luna.h"
#include "wpm_counter.h"

struct luna_wpm_status_state {
    uint8_t wpm;
};

static struct luna_wpm_status_state luna_wpm_status_get_state(const zmk_event_t *eh) {
    // eh is NULL for the initial fetch from widget_luna_init(): read the current WPM instead.
    const struct nice_beetle_wpm_changed *ev = (eh != NULL) ? as_nice_beetle_wpm_changed(eh) : NULL;
    return (struct luna_wpm_status_state){.wpm = (ev != NULL) ? ev->state
                                                              : nice_beetle_wpm_get_state()};
};

static void luna_wpm_status_update_cb(struct luna_wpm_status_state state) {
    beedly_slot_set_wpm(state.wpm);
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_luna, struct luna_wpm_status_state, luna_wpm_status_update_cb,
                            luna_wpm_status_get_state)

ZMK_SUBSCRIPTION(widget_luna, nice_beetle_wpm_changed);

int zmk_widget_luna_init(struct zmk_widget_luna *widget, lv_obj_t *parent) {
    widget->obj = beedly_slot_init(parent);

    widget_luna_init();

    return 0;
}

lv_obj_t *zmk_widget_luna_obj(struct zmk_widget_luna *widget) { return widget->obj; }
