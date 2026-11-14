/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/*
 * Parameters of the `&beedly` behaviour (compatible "zmk,behavior-beedly", one binding cell).
 *
 * Keymap-facing: bind these on the central (left) half's keymap. They run on the central only
 * (behaviour locality "central") and drive the menu drawn on the left screen.
 *   &beedly BEEDLY_OPEN    activate the BEEDLY layer, cursor back to PET
 *   &beedly BEEDLY_UP      cursor up   (PET <- FEED <- SAVE, stops at PET)
 *   &beedly BEEDLY_DOWN    cursor down (PET -> FEED -> SAVE, stops at SAVE)
 *   &beedly BEEDLY_ENTER   send the selected action to the right half; SAVE also leaves the layer
 */
#define BEEDLY_OPEN 0
#define BEEDLY_UP 1
#define BEEDLY_DOWN 2
#define BEEDLY_ENTER 3

/*
 * Remote actions: sent by the central to the peripheral (right) half over the split link by
 * BEEDLY_ENTER, run there by the same behaviour. Never bind these in a keymap.
 */
#define BEEDLY_PET 16
#define BEEDLY_FEED 17
#define BEEDLY_SAVE 18
