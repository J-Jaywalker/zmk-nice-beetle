/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>
#include <zmk/event_manager.h>

/*
 * The WPM the central screen shows (number, graph, Beedly Idle/Walk/Sprint).
 *
 * CONFIG_NICE_BEETLE_WPM_TYPING_ONLY=y (default): our own counter, ZMK's algorithm (count key
 * releases, 5 keys per word, recomputed every second, counters restarted every 5 s) but only
 * typing keys count: letters, digits, symbols/punctuation, Space, Tab, Enter and the keypad's
 * digits/operators/Enter. Not counted: modifiers, Backspace/Delete, Esc, arrows/navigation,
 * F-keys, lock keys, media/consumer keys and behaviour-only keys (they send no keycode).
 *
 * =n: ZMK's own counter (app/src/wpm.c, every key release counts), passed through unchanged.
 *
 * Either way the widgets read nice_beetle_wpm_get_state() and subscribe to
 * nice_beetle_wpm_changed, never to ZMK's zmk_wpm_state_changed directly.
 */

struct nice_beetle_wpm_changed {
    uint8_t state;
};

ZMK_EVENT_DECLARE(nice_beetle_wpm_changed);

uint8_t nice_beetle_wpm_get_state(void);
