#include <zephyr/kernel.h>
#include "util.h"
#include <ctype.h>
#include <string.h>

void to_uppercase(char *str) {
    for (int i = 0; str[i] != '\0'; i++) {
        str[i] = toupper((unsigned char)str[i]);
    }
}

void init_canvas(lv_obj_t *canvas, uint8_t *cbuf) {
    lv_canvas_set_buffer(canvas, cbuf, CANVAS_HEIGHT, CANVAS_HEIGHT, CANVAS_COLOR_FORMAT);
    lv_canvas_fill_bg(canvas, LVGL_BACKGROUND, LV_OPA_COVER);
}

/*
 * Rotate the whole square canvas 90 degrees clockwise: out(x, y) = in(y, H - 1 - x).
 * Same result as LVGL 8's lv_canvas_transform(canvas, img, 900, ..., pivot = centre); the
 * portrait drawing in columns 0..67 ends up in rows 0..67, the visible 160x68 area.
 * (LV_DISPLAY_ROTATION_270 is LVGL's name for this direction, as in ZMK's nice_view util.c.)
 */
void rotate_canvas(lv_obj_t *canvas) {
    static uint8_t buf_copy[CANVAS_HEIGHT * CANVAS_HEIGHT];
    lv_draw_buf_t *draw_buf = lv_canvas_get_draw_buf(canvas);
    const uint32_t stride = draw_buf->header.stride;

    BUILD_ASSERT(sizeof(buf_copy) <= CANVAS_BUF_SIZE, "canvas buffer too small");
    __ASSERT_NO_MSG(stride == CANVAS_HEIGHT);

    memcpy(buf_copy, draw_buf->data, sizeof(buf_copy));
    lv_draw_sw_rotate(buf_copy, draw_buf->data, CANVAS_HEIGHT, CANVAS_HEIGHT, stride, stride,
                      LV_DISPLAY_ROTATION_270, CANVAS_COLOR_FORMAT);
    lv_obj_invalidate(canvas);
}

void draw_background(lv_obj_t *canvas) {
    lv_draw_rect_dsc_t rect_black_dsc;
    init_rect_dsc(&rect_black_dsc, LVGL_BACKGROUND);

    canvas_draw_rect(canvas, 0, 0, CANVAS_WIDTH, CANVAS_HEIGHT, &rect_black_dsc);
}

void init_label_dsc(lv_draw_label_dsc_t *label_dsc, lv_color_t color, const lv_font_t *font,
                    lv_text_align_t align) {
    lv_draw_label_dsc_init(label_dsc);
    label_dsc->color = color;
    label_dsc->font = font;
    label_dsc->align = align;
}

void init_rect_dsc(lv_draw_rect_dsc_t *rect_dsc, lv_color_t bg_color) {
    lv_draw_rect_dsc_init(rect_dsc);
    rect_dsc->bg_color = bg_color;
}

void init_line_dsc(lv_draw_line_dsc_t *line_dsc, lv_color_t color, uint8_t width) {
    lv_draw_line_dsc_init(line_dsc);
    line_dsc->color = color;
    line_dsc->width = width;
}

void canvas_draw_line(lv_obj_t *canvas, const lv_point_t points[], uint32_t point_cnt,
                      lv_draw_line_dsc_t *draw_dsc) {
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    for (uint32_t i = 1; i < point_cnt; ++i) {
        draw_dsc->p1.x = points[i - 1].x;
        draw_dsc->p1.y = points[i - 1].y;
        draw_dsc->p2.x = points[i].x;
        draw_dsc->p2.y = points[i].y;
        lv_draw_line(&layer, draw_dsc);
    }

    lv_canvas_finish_layer(canvas, &layer);
}

void canvas_draw_rect(lv_obj_t *canvas, int32_t x, int32_t y, int32_t w, int32_t h,
                      lv_draw_rect_dsc_t *draw_dsc) {
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    lv_area_t coords = {x, y, x + w - 1, y + h - 1};
    lv_draw_rect(&layer, draw_dsc, &coords);

    lv_canvas_finish_layer(canvas, &layer);
}

/* Like LVGL 8's canvas_draw_text(): the text box is x .. x + max_w - 1, from y to the bottom. */
void canvas_draw_text(lv_obj_t *canvas, int32_t x, int32_t y, int32_t max_w,
                      lv_draw_label_dsc_t *draw_dsc, const char *txt) {
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    draw_dsc->text = txt;
    lv_area_t coords = {x, y, x + max_w - 1, CANVAS_HEIGHT - 1};
    lv_draw_label(&layer, draw_dsc, &coords);

    lv_canvas_finish_layer(canvas, &layer);
}

void canvas_draw_img(lv_obj_t *canvas, int32_t x, int32_t y, const lv_image_dsc_t *src,
                     lv_draw_image_dsc_t *draw_dsc) {
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);

    draw_dsc->src = src;
    lv_area_t coords = {x, y, x + src->header.w - 1, y + src->header.h - 1};
    lv_draw_image(&layer, draw_dsc, &coords);

    lv_canvas_finish_layer(canvas, &layer);
}
