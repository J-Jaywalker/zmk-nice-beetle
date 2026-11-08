/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/bluetooth/services/bas.h>
#include <zephyr/kernel.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include "hid_indicators.h"
#include <zmk/display.h>
#include <zmk/event_manager.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/hid_indicators.h>
// #include "../assets/custom_fonts.h"

#define LED_NLCK 0x01
#define LED_CLCK 0x02
#define LED_SLCK 0x04

// TODO:
// #ifndef CONFIG_NICE_OLED_WIDGET_HID_INDICATORS_LUNA_ANIMATION_MS
// #define CONFIG_NICE_OLED_WIDGET_HID_INDICATORS_LUNA_ANIMATION_MS 960
// #define CONFIG_NICE_OLED_WIDGET_HID_INDICATORS_LUNA_ANIMATION_MS 200
// #endif

LV_IMAGE_DECLARE(dog_bark1_90);
LV_IMAGE_DECLARE(dog_bark2_90);

/* Arreglo de pointers a los frames de "Luna" (definidos en luna.c) */
// const lv_image_dsc_t *luna_imgs_bark_90[] = {&dog_sit1, &dog_sit2,  &dog_walk1,
// &dog_walk2, &dog_run1, &dog_run2, &dog_bark1, &dog_bark2, &dog_sneak1,
// &dog_sneak2};
const lv_image_dsc_t *luna_imgs_bark_90[] = {&dog_bark1_90, &dog_bark2_90};
static lv_obj_t *hid_anim =
    NULL; // Variable estática para almacenar el objeto animado

struct hid_indicators_state {
  uint8_t hid_indicators;
};

static sys_slist_t widgets = SYS_SLIST_STATIC_INIT(&widgets);

/* WORK FINE
static void set_hid_indicators(lv_obj_t *label, struct hid_indicators_state
state) {

    char text[14] = {};
    bool caps_lock_on = false;

    if (state.hid_indicators & LED_CLCK) {
        // strncat(text, "C", 1);
        caps_lock_on = true;
    }
    if (state.hid_indicators & LED_NLCK) {
        // strncat(text, "N", 1);
        caps_lock_on = true;
    }
    if (state.hid_indicators & LED_SLCK) {
        // strncat(text, "S", 1);
        caps_lock_on = true;
    }
    if (caps_lock_on) {
        // strncat(text, "CAPS", 4);
        strncat(text, ".", 1);
    }

    lv_label_set_text(label, text);
    lv_obj_align(label, LV_ALIGN_OUT_TOP_LEFT, 3, 22); // point
}
*/

static void set_hid_indicators(lv_obj_t *label,
                               struct hid_indicators_state state) {
  // char text[14] = "";

  // Construir el texto según los indicadores activos.
  // Se agregan las letras correspondientes si cada indicador está activo.

#if IS_ENABLED(CONFIG_NICE_OLED_WIDGET_HID_INDICATORS_LUNA_ONLY_CAPSLOCK)
  if (state.hid_indicators & LED_CLCK) {
#else
  if (state.hid_indicators & (LED_CLCK | LED_NLCK | LED_SLCK)) {
#endif
    lv_label_set_text(label, "");
    // WORK FINE!!!
    // strncat(text, "CAPS", sizeof(text) - strlen(text) - 1);
    // strncat(text, ".", sizeof(text) - strlen(text) - 1);

    if (!hid_anim) { // Si no existe aún, creamos la animación

      hid_anim = lv_animimg_create(label);
      lv_obj_center(hid_anim);

      // lv_animimg_set_src(hid_anim, (const void **)luna_imgs_bark_90, 10);
      lv_animimg_set_src(hid_anim, (const void **)luna_imgs_bark_90, 2);
      lv_animimg_set_duration(
          hid_anim, CONFIG_NICE_OLED_WIDGET_HID_INDICATORS_LUNA_ANIMATION_MS);
      lv_animimg_set_repeat_count(hid_anim, LV_ANIM_REPEAT_INFINITE);
      lv_animimg_start(hid_anim);

      lv_obj_align(hid_anim, LV_ALIGN_TOP_LEFT, 100, 15);
      // lv_obj_align(hid_anim, LV_ALIGN_TOP_LEFT, 36, 0);
      // lv_obj_align(hid_anim, LV_ALIGN_TOP_LEFT, 33, 3);
    }
  } else {
    // Si LED_CLCK no está activo, y se había creado la animación, la eliminamos
    if (hid_anim) {
      lv_obj_delete(hid_anim);
      hid_anim = NULL;
    }
    lv_label_set_text(label, "");
  }

  // if (state.hid_indicators & LED_NLCK) {
  // strncat(text, "NLCK", sizeof(text) - strlen(text) - 1);
  // }
  // if (state.hid_indicators & LED_SLCK) {
  // strncat(text, "SLCK", sizeof(text) - strlen(text) - 1);
  // }
  // if (strlen(text) == 0) {
  // strcpy(text, "");
  // }

  // 1. &custom_font_outline = white 2. &custom_font_shadow = black 3.
  // &custom_font_44 = white &custom_font_44, &pixel_operator_mono_8,
  // &pixel_operator_mono_12, &pixel_operator_mono
  // lv_obj_set_style_text_font(label, &custom_font_22, LV_PART_MAIN |
  // LV_STATE_DEFAULT); params: obj, align, y, x, default: 33, 11
  // lv_obj_align(label, LV_ALIGN_OUT_TOP_LEFT, 30, -4); // big 44 words
  // lv_obj_align(label, LV_ALIGN_OUT_TOP_LEFT, 30, 6); // big 22 words

  // WORK FINE!!!
  // lv_obj_set_style_text_font(label, &custom_font_44, LV_PART_MAIN |
  // LV_STATE_DEFAULT); lv_label_set_text(label, text); lv_obj_align(label,
  // LV_ALIGN_OUT_TOP_LEFT, 3, -4); // big point
}

void hid_indicators_update_cb(struct hid_indicators_state state) {
  struct zmk_widget_hid_indicators *widget;
  SYS_SLIST_FOR_EACH_CONTAINER(&widgets, widget, node) {
    set_hid_indicators(widget->obj, state);
  }
}

static struct hid_indicators_state
hid_indicators_get_state(const zmk_event_t *eh) {
  // eh is NULL for the initial fetch from widget_hid_indicators_init(): read
  // the current indicators of the active endpoint instead.
  const struct zmk_hid_indicators_changed *ev =
      (eh != NULL) ? as_zmk_hid_indicators_changed(eh) : NULL;
  return (struct hid_indicators_state){
      .hid_indicators = (ev != NULL) ? ev->indicators
                                     : zmk_hid_indicators_get_current_profile(),
  };
}

ZMK_DISPLAY_WIDGET_LISTENER(widget_hid_indicators, struct hid_indicators_state,
                            hid_indicators_update_cb, hid_indicators_get_state)

ZMK_SUBSCRIPTION(widget_hid_indicators, zmk_hid_indicators_changed);

int zmk_widget_hid_indicators_init(struct zmk_widget_hid_indicators *widget,
                                   lv_obj_t *parent) {
  widget->obj = lv_label_create(parent);

  sys_slist_append(&widgets, &widget->node);

  widget_hid_indicators_init();

  return 0;
}

lv_obj_t *
zmk_widget_hid_indicators_obj(struct zmk_widget_hid_indicators *widget) {
  return widget->obj;
}
