/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Central-screen WPM counter, see wpm_counter.h. The typing-only counter is ZMK's
 * app/src/wpm.c algorithm (same constants, same arithmetic, same 1 s timer and 5 s reset) with a
 * keycode filter in front of it; ZMK's own counter keeps running untouched (CONFIG_ZMK_WPM is
 * still selected) but the widgets no longer listen to it, so nothing is raised twice.
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/event_manager.h>

#include "wpm_counter.h"

ZMK_EVENT_IMPL(nice_beetle_wpm_changed);

#if IS_ENABLED(CONFIG_NICE_BEETLE_WPM_TYPING_ONLY)

#include <dt-bindings/zmk/hid_usage_pages.h>
#include <zmk/events/keycode_state_changed.h>

#define WPM_UPDATE_INTERVAL_SECONDS 1
#define WPM_RESET_INTERVAL_SECONDS 5
#define CHARS_PER_WORD 5.0

static uint8_t wpm_state;
static uint8_t last_wpm_state;
static uint8_t wpm_update_counter;
static uint32_t key_pressed_count;

/* HID keyboard page usages that are typing (see wpm_counter.h). */
static bool is_typing_key(uint16_t page, uint32_t id) {
    if (page != HID_USAGE_KEY) {
        return false; /* consumer (media) keys etc. */
    }
    return (id >= 0x04 && id <= 0x28) || /* a..z, 1..0, Enter */
           id == 0x2B || id == 0x2C ||   /* Tab, Space */
           (id >= 0x2D && id <= 0x38) || /* - = [ ] \ #~ ; ' ` , . / */
           id == 0x64 ||                 /* non-US \| */
           (id >= 0x54 && id <= 0x63) || /* keypad / * - + Enter 1..9 0 . */
           id == 0x67 || id == 0x85;     /* keypad =, keypad , */
    /* not: 0x29 Esc, 0x2A Backspace, 0x39 Caps Lock, 0x3A.. F-keys, Print/Scroll/Pause,
     * Insert/Home/PgUp/Delete/End/PgDn, arrows, 0x53 Num Lock, 0xE0..0xE7 modifiers, ... */
}

static int typing_wpm_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    // count only key up events, like ZMK
    if (ev && !ev->state && is_typing_key(ev->usage_page, ev->keycode)) {
        key_pressed_count++;
        LOG_DBG("typing key 0x%02x, count %d", (unsigned)ev->keycode, key_pressed_count);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(nice_beetle_wpm, typing_wpm_listener);
ZMK_SUBSCRIPTION(nice_beetle_wpm, zmk_keycode_state_changed);

static void typing_wpm_work_handler(struct k_work *work) {
    wpm_update_counter++;
    wpm_state = (key_pressed_count / CHARS_PER_WORD) /
                (wpm_update_counter * WPM_UPDATE_INTERVAL_SECONDS / 60.0);

    if (last_wpm_state != wpm_state) {
        raise_nice_beetle_wpm_changed((struct nice_beetle_wpm_changed){.state = wpm_state});
        last_wpm_state = wpm_state;
    }

    if (wpm_update_counter >= WPM_RESET_INTERVAL_SECONDS) {
        wpm_update_counter = 0;
        key_pressed_count = 0;
    }
}

K_WORK_DEFINE(typing_wpm_work, typing_wpm_work_handler);

static void typing_wpm_expiry(struct k_timer *_timer) { k_work_submit(&typing_wpm_work); }

K_TIMER_DEFINE(typing_wpm_timer, typing_wpm_expiry, NULL);

static int typing_wpm_init(void) {
    k_timer_start(&typing_wpm_timer, K_SECONDS(WPM_UPDATE_INTERVAL_SECONDS),
                  K_SECONDS(WPM_UPDATE_INTERVAL_SECONDS));
    return 0;
}

SYS_INIT(typing_wpm_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

uint8_t nice_beetle_wpm_get_state(void) { return wpm_state; }

#else /* !CONFIG_NICE_BEETLE_WPM_TYPING_ONLY: ZMK's counter, passed through */

#include <zmk/events/wpm_state_changed.h>
#include <zmk/wpm.h>

static int zmk_wpm_passthrough(const zmk_event_t *eh) {
    const struct zmk_wpm_state_changed *ev = as_zmk_wpm_state_changed(eh);
    if (ev) {
        raise_nice_beetle_wpm_changed((struct nice_beetle_wpm_changed){.state = ev->state});
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(nice_beetle_wpm, zmk_wpm_passthrough);
ZMK_SUBSCRIPTION(nice_beetle_wpm, zmk_wpm_state_changed);

uint8_t nice_beetle_wpm_get_state(void) { return (uint8_t)zmk_wpm_get_state(); }

#endif /* CONFIG_NICE_BEETLE_WPM_TYPING_ONLY */
