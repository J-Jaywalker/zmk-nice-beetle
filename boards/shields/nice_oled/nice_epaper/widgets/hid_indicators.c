/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Host lock LEDs -> central Beedly animation. Caps Lock on shows the CAPSLOCK tag (through the
 * shared slot in beedly_slot.c; held modifiers take precedence, the WPM animation returns when it
 * goes off). This widget creates no LVGL object of its own.
 *
 * Num Lock / Scroll Lock: the Luna dog used to bark for any lock key unless
 * CONFIG_NICE_OLED_WIDGET_HID_INDICATORS_LUNA_ONLY_CAPSLOCK=y. Beedly has no Num/Scroll Lock
 * animation and the CAPSLOCK one would be wrong for them (Num Lock is on most of the time on
 * many PCs, which would hide the WPM animation for good), so only Caps Lock is shown and that
 * option no longer changes anything here.
 * CONFIG_NICE_OLED_WIDGET_HID_INDICATORS_LUNA=n disables the animation.
 */

#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/hid_indicators.h>

#include "beedly_slot.h"
#include "hid_indicators.h"

#define LED_NLCK 0x01
#define LED_CLCK 0x02
#define LED_SLCK 0x04

struct hid_indicators_state {
    uint8_t hid_indicators;
};

static void hid_indicators_update_cb(struct hid_indicators_state state) {
#if IS_ENABLED(CONFIG_NICE_OLED_WIDGET_HID_INDICATORS_LUNA)
    beedly_slot_set_caps_lock((state.hid_indicators & LED_CLCK) != 0);
#endif
}

static struct hid_indicators_state hid_indicators_get_state(const zmk_event_t *eh) {
    // eh is NULL for the initial fetch from widget_hid_indicators_init(): read
    // the current indicators of the active endpoint instead.
    const struct zmk_hid_indicators_changed *ev =
        (eh != NULL) ? as_zmk_hid_indicators_changed(eh) : NULL;
    return (struct hid_indicators_state){
        .hid_indicators =
            (ev != NULL) ? ev->indicators : zmk_hid_indicators_get_current_profile(),
    };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_hid_indicators, struct hid_indicators_state,
                            hid_indicators_update_cb, hid_indicators_get_state)

ZMK_SUBSCRIPTION(widget_hid_indicators, zmk_hid_indicators_changed);

int zmk_widget_hid_indicators_init(struct zmk_widget_hid_indicators *widget, lv_obj_t *parent) {
    widget->obj = beedly_slot_init(parent);

    widget_hid_indicators_init();

    return 0;
}

lv_obj_t *zmk_widget_hid_indicators_obj(struct zmk_widget_hid_indicators *widget) {
    return widget->obj;
}
