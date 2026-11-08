#pragma once

#include <stddef.h>
#include <lvgl.h>

/*
 * Beedly animation frames, defined in the generated assets/beedly_frames.c
 * (`make beedly` regenerates it from sprite_assets/). The table holds the dithered or the plain
 * frames depending on CONFIG_NICE_BEETLE_DITHER_BEEDLY.
 */
extern const lv_image_dsc_t *const beedly_frames[];
extern const size_t beedly_frame_count;
