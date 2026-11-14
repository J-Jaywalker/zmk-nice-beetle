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
 * Beedly pet, PERIPHERAL (right half) only: the only owner of the pet's state.
 *
 *   hearts, food   0..BEEDLY_PET_BAR_MAX; PET / FEED add CONFIG_NICE_BEETLE_PET_{HEARTS,FOOD}_STEP.
 *                  A full bar makes the action do nothing at all (no XP, no animation, no save).
 *                  Both decay lazily: value + uptime anchor are stored, the loss is computed from
 *                  the uptime only when the screen is redrawn, an action arrives or stats are
 *                  saved (no timer, no polling; uptime restarts at boot, so power-off time and
 *                  deep sleep do not count).
 *   xp, level      every effective PET / FEED adds CONFIG_NICE_BEETLE_PET_XP_PER_ACTION;
 *                  CONFIG_NICE_BEETLE_PET_XP_PER_LEVEL XP make a level, up to _MAX_LEVEL.
 *
 * Drawn OVER the right screen's Beedly art (animation.c's lv_animimg, panel (6, 0), 115 x 68),
 * whose bottom already shows a mock HUD: a "BEEDLY : LVL 5" pill and three bars under a heart,
 * a food and a "Zz" icon. Three small opaque L8 canvases, created once after the art so they sit
 * on top of it, replace exactly the parts that become live (upright 68 x 160 coordinates; the
 * art's icons and the pill text stay visible and keep animating):
 *
 *   level   x 53..60, y 125..129  the digit cell inside the pill ("5" -> the level, 1-2 digits)
 *   bars    x  0..67, y 145..151  hearts (x 2..21), food (x 24..43), XP to next level (x 46..65,
 *                                 under the art's "Zz" icon: there is no sleep stat)
 *   hearts  x 58..66, y  40..63   right of Beedly (never inked by any art frame); hidden unless
 *                                 a PET animation runs
 *
 * Battery (y 19..29) and connection status (y 0..15) are untouched. Static RAM ~730 bytes of
 * canvas buffers; LVGL pool: three canvas objects (no allocation while drawing: pixels are
 * written straight into the buffers).
 */
#define BEEDLY_PET_BAR_MAX 100

/* Create the overlays: after draw_animation(), on the same parent (display thread). */
void beedly_pet_init(lv_obj_t *parent);
/* Repaint the overlays from the current (decayed) state (display thread). */
void beedly_pet_render(void);

/* Behaviour side (split command context): plain state + work submit, no LVGL. */
int beedly_pet_request(uint32_t action);

/* State readers (any context; hearts/food include the decay up to now). */
uint8_t beedly_pet_hearts(void);
uint8_t beedly_pet_food(void);
uint8_t beedly_pet_level(void);
uint16_t beedly_pet_xp(void);
bool beedly_pet_dirty(void);
bool beedly_pet_animating(void);
