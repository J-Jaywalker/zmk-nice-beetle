#pragma once

/*
 * GENERATED FILE - do not edit by hand (`make beedly_left`, see beedly_luna_frames.c).
 * Central-screen Beedly animations, one per Aseprite tag of sprite_assets/ase/beedly_left.aseprite.
 * Each <tag>[] holds the tag's frames in play order, <tag>_count how many there are.
 */

#include <stddef.h>
#include <lvgl.h>

/* every frame is 32 x 32 px (stored rotated, as drawn on the panel) */
#define BEEDLY_LUNA_FRAME_W 32
#define BEEDLY_LUNA_FRAME_H 32

/* Idle: WPM below CONFIG_NICE_BEETLE_LUNA_WALK_WPM; also shown while Gui is held */
extern const lv_image_dsc_t *const beedly_luna_idle[];
extern const size_t beedly_luna_idle_count;

/* Walk: WPM from CONFIG_NICE_BEETLE_LUNA_WALK_WPM up to the sprint threshold */
extern const lv_image_dsc_t *const beedly_luna_walk[];
extern const size_t beedly_luna_walk_count;

/* Sprint: WPM at or above CONFIG_NICE_BEETLE_LUNA_SPRINT_WPM */
extern const lv_image_dsc_t *const beedly_luna_sprint[];
extern const size_t beedly_luna_sprint_count;

/* Shift: Shift held */
extern const lv_image_dsc_t *const beedly_luna_shift[];
extern const size_t beedly_luna_shift_count;

/* Ctrl: Ctrl held */
extern const lv_image_dsc_t *const beedly_luna_ctrl[];
extern const size_t beedly_luna_ctrl_count;

/* Alt: Alt held */
extern const lv_image_dsc_t *const beedly_luna_alt[];
extern const size_t beedly_luna_alt_count;

/* CAPSLOCK: Caps Lock on */
extern const lv_image_dsc_t *const beedly_luna_capslock[];
extern const size_t beedly_luna_capslock_count;
