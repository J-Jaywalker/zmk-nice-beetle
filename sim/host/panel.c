/*
 * Display post-processing stage: turns the firmware display driver's writes
 * into what a viewer sees.  Pluggable models (PANEL_EMU / --panel):
 *
 *   ideal   every write is visible immediately and exactly (1 = white,
 *           0 = black).  Default; best for pixel-exact tests.
 *   memlcd  Sharp memory LCD (the overlay's sharp,ls0xx / nice!view):
 *           - each write reaches the glass line by line while it is shifted
 *             out over SPI (timing comes from the MCU DLL, computed from the
 *             overlay's spi-max-frequency, see fake_display.c), so a frame
 *             can be caught half-updated (tearing) exactly like on hardware;
 *           - pixels keep their state between writes (memory-in-pixel, no
 *             refresh needed, no decay);
 *           - optional liquid-crystal response time (--memlcd-response-ms,
 *             default 0: NOT calibrated against a datasheet);
 *           - reflective-panel palette (approximate look, not measured);
 *           - VCOM: the firmware never toggles it with this overlay (no
 *             extcomin-gpios, VCOM bit never set in the write command), and
 *             VCOM inversion has no visible short-term effect on a working
 *             panel, so nothing is drawn for it.  Long-term DC-bias image
 *             degradation is not modelled.
 *
 * Rotation is applied after the model: the widgets draw portrait and rotate
 * the canvas onto the landscape 160x68 panel, so the default view rotation
 * (270 degrees clockwise = 90 counter-clockwise) makes the text upright.
 */
#include <stdlib.h>
#include <string.h>
#include "host.h"

void panel_init(struct panel *p, int w, int h, const struct panel_model *m) {
    if (p->model_state) {
        free(p->model_state);
    }
    memset(p, 0, sizeof(*p));
    p->w = w;
    p->h = h;
    p->model = m;
    /* ls0xx_init() ends with the panel's "all clear" command: every pixel white */
    memset(p->fb, 1, (size_t)w * h);
}

/* ------------------------------------------------------------ ideal */

static void ideal_on_write(struct panel *p, int64_t start_us, const uint8_t *fb, int y0, int y1,
                           int xfer_us) {
    (void)start_us;
    (void)xfer_us;
    for (int y = y0; y <= y1; y++) {
        memcpy(p->fb + y * p->w, fb + y * p->w, p->w);
    }
}

static void ideal_render(struct panel *p, int64_t t_us, uint8_t *grey) {
    (void)t_us;
    for (int i = 0; i < p->w * p->h; i++) {
        grey[i] = p->fb[i] ? 255 : 0;
    }
}

static bool ideal_busy(struct panel *p, int64_t t_us) { return false; }

const struct panel_model panel_model_ideal = {"ideal", ideal_on_write, ideal_render, ideal_busy};

/* ------------------------------------------------------- memory LCD */

int memlcd_response_ms = 0;
#define MEMLCD_WHITE 214 /* approximate reflective look */
#define MEMLCD_BLACK 40
#define MEMLCD_QUEUE 64

struct memlcd_write {
    int64_t start_us;
    int xfer_us;
    int y0, y1;
    int next_line; /* first line not yet latched */
    uint8_t *rows;  /* copy of rows y0..y1 */
};

struct memlcd {
    struct memlcd_write q[MEMLCD_QUEUE];
    int head, n;
    uint8_t *glass;       /* latched pixel state (1 white) */
    int64_t *changed_us;  /* when each pixel last flipped */
};

static struct memlcd *ml(struct panel *p) {
    if (!p->model_state) {
        struct memlcd *m = calloc(1, sizeof(*m));
        m->glass = calloc(p->w * p->h, 1);
        memcpy(m->glass, p->fb, (size_t)p->w * p->h); /* state at power-up / after clear */
        m->changed_us = calloc(p->w * p->h, sizeof(int64_t));
        for (int i = 0; i < p->w * p->h; i++) {
            m->changed_us[i] = INT64_MIN / 2;
        }
        p->model_state = m;
    }
    return p->model_state;
}

/* time at which line i (0-based within the write) has been fully clocked in:
 * bytes = 1 command + 22 per line (address + data + dummy) */
static int64_t line_done_us(const struct memlcd_write *w, int i) {
    int n = w->y1 - w->y0 + 1;
    int64_t bytes_total = 2 + 22LL * n, bytes = 1 + 22LL * (i + 1);
    return w->start_us + (int64_t)w->xfer_us * bytes / bytes_total;
}

static void latch_line(struct panel *p, struct memlcd *m, const uint8_t *row, int y, int64_t t) {
    for (int x = 0; x < p->w; x++) {
        int i = y * p->w + x;
        if (m->glass[i] != row[x]) {
            m->glass[i] = row[x];
            m->changed_us[i] = t;
        }
    }
}

static void memlcd_advance(struct panel *p, int64_t t_us) {
    struct memlcd *m = ml(p);
    while (m->n > 0) {
        struct memlcd_write *w = &m->q[m->head];
        int n = w->y1 - w->y0 + 1;
        while (w->next_line < n && line_done_us(w, w->next_line) <= t_us) {
            latch_line(p, m, w->rows + w->next_line * p->w, w->y0 + w->next_line,
                       line_done_us(w, w->next_line));
            w->next_line++;
        }
        if (w->next_line < n) {
            break;
        }
        free(w->rows);
        w->rows = NULL;
        m->head = (m->head + 1) % MEMLCD_QUEUE;
        m->n--;
    }
}

static void memlcd_on_write(struct panel *p, int64_t start_us, const uint8_t *fb, int y0, int y1,
                            int xfer_us) {
    struct memlcd *m = ml(p);
    for (int y = y0; y <= y1; y++) {
        memcpy(p->fb + y * p->w, fb + y * p->w, p->w);
    }
    if (m->n == MEMLCD_QUEUE) { /* should not happen: flush the oldest */
        memlcd_advance(p, INT64_MAX / 4);
    }
    struct memlcd_write *w = &m->q[(m->head + m->n) % MEMLCD_QUEUE];
    w->start_us = start_us;
    w->xfer_us = xfer_us;
    w->y0 = y0;
    w->y1 = y1;
    w->next_line = 0;
    w->rows = malloc((size_t)(y1 - y0 + 1) * p->w);
    memcpy(w->rows, fb + y0 * p->w, (size_t)(y1 - y0 + 1) * p->w);
    m->n++;
}

static void memlcd_render(struct panel *p, int64_t t_us, uint8_t *grey) {
    struct memlcd *m = ml(p);
    memlcd_advance(p, t_us);
    int64_t resp = (int64_t)memlcd_response_ms * 1000;
    for (int i = 0; i < p->w * p->h; i++) {
        int to = m->glass[i] ? MEMLCD_WHITE : MEMLCD_BLACK;
        int64_t dt = t_us - m->changed_us[i];
        if (resp > 0 && dt < resp) {
            int from = m->glass[i] ? MEMLCD_BLACK : MEMLCD_WHITE;
            to = from + (int)((to - from) * dt / resp);
        }
        grey[i] = (uint8_t)to;
    }
}

static bool memlcd_busy(struct panel *p, int64_t t_us) {
    struct memlcd *m = ml(p);
    if (m->n > 0) {
        return true;
    }
    if (memlcd_response_ms > 0) {
        int64_t resp = (int64_t)memlcd_response_ms * 1000;
        for (int i = 0; i < p->w * p->h; i++) {
            if (t_us - m->changed_us[i] < resp) {
                return true;
            }
        }
    }
    return false;
}

const struct panel_model panel_model_memlcd = {"memlcd", memlcd_on_write, memlcd_render,
                                               memlcd_busy};

const struct panel_model *panel_model_by_name(const char *name) {
    if (!strcmp(name, "ideal") || !strcmp(name, "0")) {
        return &panel_model_ideal;
    }
    if (!strcmp(name, "memlcd") || !strcmp(name, "1")) {
        return &panel_model_memlcd;
    }
    return NULL;
}

/* ------------------------------------------------------- rotation/blit */

void panel_rotated_size(int w, int h, int rot, int *ow, int *oh) {
    if (rot == 90 || rot == 270) {
        *ow = h;
        *oh = w;
    } else {
        *ow = w;
        *oh = h;
    }
}

void panel_blit(struct panel *p, int64_t t_us, int rot, int scale, uint8_t *dst, int dst_w, int dx,
                int dy) {
    static uint8_t grey[HOST_MAX_FB];
    p->model->render(p, t_us, grey);
    int ow, oh;
    panel_rotated_size(p->w, p->h, rot, &ow, &oh);
    for (int oy = 0; oy < oh; oy++) {
        for (int ox = 0; ox < ow; ox++) {
            int sx, sy;
            switch (rot) {
            case 90: /* clockwise */
                sx = oy;
                sy = p->h - 1 - ox;
                break;
            case 180:
                sx = p->w - 1 - ox;
                sy = p->h - 1 - oy;
                break;
            case 270:
                sx = p->w - 1 - oy;
                sy = ox;
                break;
            default:
                sx = ox;
                sy = oy;
            }
            uint8_t v = grey[sy * p->w + sx];
            for (int yy = 0; yy < scale; yy++) {
                uint8_t *d = dst + (size_t)(dy + oy * scale + yy) * dst_w + dx + ox * scale;
                memset(d, v, scale);
            }
        }
    }
}
