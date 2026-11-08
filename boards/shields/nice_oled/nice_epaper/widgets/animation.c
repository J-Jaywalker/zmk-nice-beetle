#include <stdlib.h>
#include <zephyr/kernel.h>
#include "animation.h"
#include "../assets/beedly_frames.h"

/*
 * The Beedly frames and their count come from the generated assets/beedly_frames.c
 * (`make beedly`), so adding or removing frames needs no change here.
 */
void draw_animation(lv_obj_t *canvas, struct zmk_widget_screen *widget) {
#if IS_ENABLED(CONFIG_NICE_OLED_GEM_ANIMATION)
    lv_obj_t *art = lv_animimg_create(widget->obj);
    lv_obj_center(art);

    lv_animimg_set_src(art, (const void **)beedly_frames, beedly_frame_count);
    lv_animimg_set_duration(art, CONFIG_NICE_OLED_GEM_ANIMATION_MS);
    lv_animimg_set_repeat_count(art, LV_ANIM_REPEAT_INFINITE);
    lv_animimg_start(art);
#else
    lv_obj_t *art = lv_image_create(widget->obj);

    srand(k_uptime_get_32());
    int random_index = rand() % beedly_frame_count;

    lv_image_set_src(art, beedly_frames[random_index]);
#endif

    lv_obj_align(art, LV_ALIGN_TOP_LEFT, 6, 0);
}
