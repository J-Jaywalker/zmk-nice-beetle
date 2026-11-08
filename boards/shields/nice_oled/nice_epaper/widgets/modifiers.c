/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Held modifiers -> central Beedly animation (Shift / Ctrl / Alt tags; Gui shows Idle). This widget
 * creates no LVGL object of its own: it reports the explicit modifiers to the shared slot
 * (beedly_slot.c), which applies the precedence Gui > Alt > Ctrl > Shift > Caps Lock > WPM and
 * returns to the WPM animation when the modifiers are released.
 * CONFIG_NICE_OLED_WIDGET_MODIFIERS_INDICATORS_LUNA=n disables the animations.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>

#include "beedly_slot.h"
#include "modifiers.h"

struct modifiers_state {
    uint8_t modifiers;
};

static void modifiers_update_cb(struct modifiers_state ignored) {
    // Read the modifiers when the display thread runs the update rather than when the event is
    // raised: the widget listener may see a keycode event before hid_listener has applied it to
    // the HID report (the old modifiers.c did the same).
#if IS_ENABLED(CONFIG_NICE_OLED_WIDGET_MODIFIERS_INDICATORS_LUNA)
    beedly_slot_set_mods(zmk_hid_get_explicit_mods());
#endif
}

static struct modifiers_state modifiers_get_state(const zmk_event_t *eh) {
    return (struct modifiers_state){.modifiers = zmk_hid_get_explicit_mods()};
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_modifiers, struct modifiers_state, modifiers_update_cb,
                            modifiers_get_state)
ZMK_SUBSCRIPTION(widget_modifiers, zmk_keycode_state_changed);

int zmk_widget_modifiers_init(struct zmk_widget_modifiers *widget, lv_obj_t *parent) {
    widget->obj = beedly_slot_init(parent);
    widget_modifiers_init();
    return 0;
}
