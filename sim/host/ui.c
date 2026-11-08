/*
 * Simulator host: native SDL2 window.
 *
 *   - both nice!view panels side by side (central left, peripheral right)
 *   - status column (time, layer, WPM, mods, output, BLE, LEDs, activity...)
 *   - clickable on-screen 42-key Corne (left click = press while held,
 *     right click = latch / unlatch a hold)
 *   - PC keyboard mapped onto the Corne by scancode (layout independent)
 *   - control buttons, also reachable through F-keys and the nav cluster
 *
 * Text is rendered with Windows GDI into a glyph atlas (SDL_ttf is not
 * available), then drawn from an SDL texture with colour modulation.
 * Needs -lgdi32 at link time.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "host.h"

/* ------------------------------------------------------------ colours */

typedef struct {
    uint8_t r, g, b, a;
} rgba;

static const rgba C_BG = {24, 26, 31, 255};
static const rgba C_PANEL_BG = {32, 35, 42, 255};
static const rgba C_BEZEL = {12, 12, 14, 255};
static const rgba C_BEZEL_EDGE = {58, 62, 72, 255};
static const rgba C_TEXT = {214, 218, 226, 255};
static const rgba C_DIM = {130, 137, 150, 255};
static const rgba C_HEAD = {120, 180, 255, 255};
static const rgba C_GOOD = {110, 210, 130, 255};
static const rgba C_BAD = {240, 110, 100, 255};
static const rgba C_WARN = {240, 190, 90, 255};
static const rgba C_KEY = {44, 48, 57, 255};
static const rgba C_KEY_EDGE = {70, 76, 90, 255};
static const rgba C_KEY_DOWN = {64, 128, 230, 255};
static const rgba C_KEY_LATCH = {176, 120, 40, 255};
static const rgba C_KEY_LATCH_DOWN = {214, 150, 50, 255};
static const rgba C_BTN = {48, 53, 64, 255};
static const rgba C_BTN_HOT = {62, 69, 84, 255};
static const rgba C_BTN_ON = {40, 92, 70, 255};

/* ------------------------------------------------------------ SDL state */

static SDL_Window *win;
static SDL_Renderer *ren;
static int win_w, win_h;

static void set_col(rgba c) { SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a); }

static void fill(int x, int y, int w, int h, rgba c) {
    SDL_Rect r = {x, y, w, h};
    set_col(c);
    SDL_RenderFillRect(ren, &r);
}

static void frame_rect(int x, int y, int w, int h, rgba c) {
    SDL_Rect r = {x, y, w, h};
    set_col(c);
    SDL_RenderDrawRect(ren, &r);
}

static bool in_rect(int px, int py, const SDL_Rect *r) {
    return px >= r->x && py >= r->y && px < r->x + r->w && py < r->y + r->h;
}

/* ------------------------------------------------------------ GDI fonts */

struct font {
    SDL_Texture *tex;
    int cw, ch; /* fixed advance and line height */
};

static struct font F_BIG, F_SMALL;

#define ATLAS_COLS 16
#define ATLAS_ROWS 6 /* 96 cells for ASCII 32..127 */

static bool font_build(struct font *f, int px, bool bold) {
    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) {
        return false;
    }
    HFONT hf = CreateFontA(-px, 0, 0, 0, bold ? FW_BOLD : FW_NORMAL, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                           FIXED_PITCH | FF_MODERN, "Consolas");
    if (!hf) {
        DeleteDC(dc);
        return false;
    }
    HGDIOBJ old_font = SelectObject(dc, hf);
    TEXTMETRICA tm;
    SIZE sz;
    GetTextMetricsA(dc, &tm);
    GetTextExtentPoint32A(dc, "M", 1, &sz);
    f->cw = sz.cx > 0 ? sz.cx : px / 2;
    f->ch = tm.tmHeight > 0 ? tm.tmHeight : px;

    int aw = ATLAS_COLS * f->cw, ah = ATLAS_ROWS * f->ch;
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = aw;
    bi.bmiHeader.biHeight = -ah; /* top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bm || !bits) {
        SelectObject(dc, old_font);
        DeleteObject(hf);
        DeleteDC(dc);
        return false;
    }
    HGDIOBJ old_bm = SelectObject(dc, bm);
    memset(bits, 0, (size_t)aw * ah * 4);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkColor(dc, RGB(0, 0, 0));
    SetBkMode(dc, OPAQUE);
    for (int c = 32; c < 127; c++) {
        int i = c - 32;
        char ch = (char)c;
        TextOutA(dc, (i % ATLAS_COLS) * f->cw, (i / ATLAS_COLS) * f->ch, &ch, 1);
    }
    GdiFlush();

    SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, aw, ah, 32, SDL_PIXELFORMAT_ARGB8888);
    bool ok = false;
    if (s) {
        const uint32_t *src = bits;
        for (int y = 0; y < ah; y++) {
            uint32_t *dst = (uint32_t *)((uint8_t *)s->pixels + (size_t)y * s->pitch);
            for (int x = 0; x < aw; x++) {
                uint32_t p = src[(size_t)y * aw + x];
                uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
                uint32_t a = (r + g + b) / 3;
                dst[x] = (a << 24) | 0x00FFFFFF;
            }
        }
        f->tex = SDL_CreateTextureFromSurface(ren, s);
        if (f->tex) {
            SDL_SetTextureBlendMode(f->tex, SDL_BLENDMODE_BLEND);
            ok = true;
        }
        SDL_FreeSurface(s);
    }
    SelectObject(dc, old_bm);
    SelectObject(dc, old_font);
    DeleteObject(bm);
    DeleteObject(hf);
    DeleteDC(dc);
    return ok;
}

/* draw at most max_chars characters (max_chars < 0: all); returns width */
static int text_n(const struct font *f, int x, int y, rgba c, const char *s, int max_chars) {
    if (!f->tex || !s) {
        return 0;
    }
    SDL_SetTextureColorMod(f->tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(f->tex, c.a);
    int n = 0;
    for (const char *p = s; *p && (max_chars < 0 || n < max_chars); p++, n++) {
        int ch = (unsigned char)*p;
        if (ch < 32 || ch > 126) {
            ch = '?';
        }
        if (ch != ' ') {
            int i = ch - 32;
            SDL_Rect src = {(i % ATLAS_COLS) * f->cw, (i / ATLAS_COLS) * f->ch, f->cw, f->ch};
            SDL_Rect dst = {x + n * f->cw, y, f->cw, f->ch};
            SDL_RenderCopy(ren, f->tex, &src, &dst);
        }
    }
    return n * f->cw;
}

static int text(const struct font *f, int x, int y, rgba c, const char *s) {
    return text_n(f, x, y, c, s, -1);
}

static int textf(const struct font *f, int x, int y, rgba c, const char *fmt, ...)
    __attribute__((format(printf, 5, 6)));
static int textf(const struct font *f, int x, int y, rgba c, const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return text(f, x, y, c, buf);
}

static int text_w(const struct font *f, const char *s) { return (int)strlen(s) * f->cw; }

/* ------------------------------------------------------------ sim clock */

static const int speeds[] = {1, 2, 5, 10, 30};
#define NSPEEDS ((int)(sizeof(speeds) / sizeof(speeds[0])))
static int speed_idx;
static bool paused;
static double sim_target; /* ms */
#define MAX_CATCHUP_MS 3000

static bool show_help;
static char toast[256];
static uint32_t toast_until;

static void set_toast(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_toast(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(toast, sizeof(toast), fmt, ap);
    va_end(ap);
    toast_until = SDL_GetTicks() + 4000;
}

/* ------------------------------------------------------------ actions */

enum action {
    A_CBATT_DN,
    A_CBATT_UP,
    A_PBATT_DN,
    A_PBATT_UP,
    A_CUSB,
    A_PUSB,
    A_BLE_PREV,
    A_BLE_NEXT,
    A_HOST_PRESENT,
    A_BOND,
    A_SPLIT,
    A_OUTPUT,
    A_CAPS,
    A_NUM,
    A_SCROLL,
    A_AUTOTYPE,
    A_SPEED,
    A_PAUSE,
    A_SKIP,
    A_SHOT,
    A_HELP,
    A_COUNT
};

static const char *action_hint[A_COUNT] = {
    [A_CBATT_DN] = "F1",      [A_CBATT_UP] = "F2",     [A_PBATT_DN] = "F3",
    [A_PBATT_UP] = "F4",      [A_CUSB] = "F5",         [A_PUSB] = "F6",
    [A_BLE_PREV] = "",        [A_BLE_NEXT] = "F7",     [A_HOST_PRESENT] = "F8",
    [A_BOND] = "F9",          [A_SPLIT] = "F10",       [A_OUTPUT] = "Home",
    [A_CAPS] = "PgUp",        [A_NUM] = "PgDn",        [A_SCROLL] = "End",
    [A_AUTOTYPE] = "Delete",  [A_SPEED] = "F11",       [A_PAUSE] = "Pause",
    [A_SKIP] = "Insert",      [A_SHOT] = "F12",        [A_HELP] = "ScrLk",
};

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int active_profile(void) { return (int)host_query(HALF_CENTRAL, SIM_Q_ACTIVE_PROFILE); }

static void make_dirs(const char *path) {
    char buf[600];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf; *p; p++) {
        if ((*p == '/' || *p == '\\') && p != buf && p[-1] != ':') {
            char save = *p;
            *p = 0;
            CreateDirectoryA(buf, NULL);
            *p = save;
        }
    }
    CreateDirectoryA(buf, NULL);
}

static void take_screenshot(void) {
    const char *dir = H.shot_dir[0] ? H.shot_dir : "sim_shots";
    make_dirs(dir);
    char path[1024];
    snprintf(path, sizeof(path), "%s/shot_%03d_%lldms.png", dir, H.shot_count, (long long)H.t);
    H.shot_count++;
    int rc = host_save_screenshot(path);
    if (rc == 0) {
        printf("screenshot: %s\n", path);
        set_toast("Saved %s", path);
    } else {
        printf("screenshot FAILED (%d): %s\n", rc, path);
        set_toast("Screenshot failed: %s", path);
    }
    fflush(stdout);
}

static void do_action(int a) {
    switch (a) {
    case A_CBATT_DN:
    case A_CBATT_UP:
    case A_PBATT_DN:
    case A_PBATT_UP: {
        int half = (a == A_CBATT_DN || a == A_CBATT_UP) ? HALF_CENTRAL : HALF_PERIPHERAL;
        int d = (a == A_CBATT_UP || a == A_PBATT_UP) ? 10 : -10;
        int v = clampi(H.half[half].battery + d, 0, 100);
        host_set_battery(half, v);
        set_toast("%s battery -> %d%% (firmware samples every 60 s; use Skip)",
                  half ? "Peripheral" : "Central", v);
        break;
    }
    case A_CUSB:
    case A_PUSB: {
        int half = a == A_CUSB ? HALF_CENTRAL : HALF_PERIPHERAL;
        host_set_usb(half, (H.half[half].usb + 1) % 3);
        break;
    }
    case A_BLE_PREV:
        host_ble_select((active_profile() + 4) % 5);
        break;
    case A_BLE_NEXT:
        host_ble_select((active_profile() + 1) % 5);
        break;
    case A_HOST_PRESENT: {
        int p = clampi(active_profile(), 0, 4);
        host_ble_host_present(p, !H.ble_host_present[p]);
        break;
    }
    case A_BOND: {
        int p = clampi(active_profile(), 0, 4);
        host_ble_bond(p, !host_query(HALF_CENTRAL, SIM_Q_PROFILE_BONDED));
        break;
    }
    case A_SPLIT:
        host_set_split(!H.split_connected);
        break;
    case A_OUTPUT:
        host_select_output(host_query(HALF_CENTRAL, SIM_Q_ENDPOINT_TRANSPORT) == 0 ? 1 : 0);
        break;
    case A_NUM:
        host_set_leds(H.leds ^ 1);
        break;
    case A_CAPS:
        host_set_leds(H.leds ^ 2);
        break;
    case A_SCROLL:
        host_set_leds(H.leds ^ 4);
        break;
    case A_AUTOTYPE: {
        static const int wpms[] = {0, 40, 80, 120};
        int next = 0;
        for (int i = 0; i < 4; i++) {
            if (H.autotype_wpm < wpms[i]) {
                next = wpms[i];
                break;
            }
        }
        host_autotype_set(next);
        break;
    }
    case A_SPEED:
        speed_idx = (speed_idx + 1) % NSPEEDS;
        break;
    case A_PAUSE:
        paused = !paused;
        break;
    case A_SKIP:
        host_step_to(H.t + 30000);
        if (sim_target < H.t) {
            sim_target = (double)H.t;
        }
        break;
    case A_SHOT:
        take_screenshot();
        break;
    case A_HELP:
        show_help = !show_help;
        break;
    }
}

static void action_label(int a, char *buf, size_t n) {
    switch (a) {
    case A_CBATT_DN: snprintf(buf, n, "C batt -10"); break;
    case A_CBATT_UP: snprintf(buf, n, "C batt +10"); break;
    case A_PBATT_DN: snprintf(buf, n, "P batt -10"); break;
    case A_PBATT_UP: snprintf(buf, n, "P batt +10"); break;
    case A_CUSB: snprintf(buf, n, "C USB: %s", host_usb_name(H.half[0].usb)); break;
    case A_PUSB: snprintf(buf, n, "P USB: %s", host_usb_name(H.half[1].usb)); break;
    case A_BLE_PREV: snprintf(buf, n, "BLE prev"); break;
    case A_BLE_NEXT: snprintf(buf, n, "BLE next"); break;
    case A_HOST_PRESENT: {
        int p = clampi(active_profile(), 0, 4);
        snprintf(buf, n, "Host %d: %s", p, H.ble_host_present[p] ? "here" : "away");
        break;
    }
    case A_BOND:
        snprintf(buf, n, "Bond: %s", host_query(0, SIM_Q_PROFILE_BONDED) ? "yes" : "no");
        break;
    case A_SPLIT: snprintf(buf, n, "Split: %s", H.split_connected ? "up" : "down"); break;
    case A_OUTPUT:
        snprintf(buf, n, "Output: %s",
                 host_query(0, SIM_Q_ENDPOINT_TRANSPORT) == 0 ? "USB" : host_query(0, SIM_Q_ENDPOINT_TRANSPORT) == 1 ? "BLE" : "none");
        break;
    case A_CAPS: snprintf(buf, n, "Caps lock"); break;
    case A_NUM: snprintf(buf, n, "Num lock"); break;
    case A_SCROLL: snprintf(buf, n, "Scroll lock"); break;
    case A_AUTOTYPE:
        if (H.autotype_wpm > 0) {
            snprintf(buf, n, "Autotype %d", H.autotype_wpm);
        } else {
            snprintf(buf, n, "Autotype off");
        }
        break;
    case A_SPEED: snprintf(buf, n, "Speed %dx", speeds[speed_idx]); break;
    case A_PAUSE: snprintf(buf, n, paused ? "Resume" : "Pause"); break;
    case A_SKIP: snprintf(buf, n, "Skip +30 s"); break;
    case A_SHOT: snprintf(buf, n, "Screenshot"); break;
    case A_HELP: snprintf(buf, n, show_help ? "Hide help" : "Help"); break;
    default: buf[0] = 0;
    }
}

static bool action_on(int a) {
    switch (a) {
    case A_CUSB: return H.half[0].usb != SIM_USB_NONE;
    case A_PUSB: return H.half[1].usb != SIM_USB_NONE;
    case A_HOST_PRESENT: return H.ble_host_present[clampi(active_profile(), 0, 4)];
    case A_BOND: return host_query(0, SIM_Q_PROFILE_BONDED) != 0;
    case A_SPLIT: return H.split_connected;
    case A_NUM: return (H.leds & 1) != 0;
    case A_CAPS: return (H.leds & 2) != 0;
    case A_SCROLL: return (H.leds & 4) != 0;
    case A_AUTOTYPE: return H.autotype_wpm > 0;
    case A_PAUSE: return paused;
    case A_HELP: return show_help;
    default: return false;
    }
}

/* F-keys / nav cluster -> action (-1: not a control key) */
static int scancode_action(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_F1: return A_CBATT_DN;
    case SDL_SCANCODE_F2: return A_CBATT_UP;
    case SDL_SCANCODE_F3: return A_PBATT_DN;
    case SDL_SCANCODE_F4: return A_PBATT_UP;
    case SDL_SCANCODE_F5: return A_CUSB;
    case SDL_SCANCODE_F6: return A_PUSB;
    case SDL_SCANCODE_F7: return A_BLE_NEXT;
    case SDL_SCANCODE_F8: return A_HOST_PRESENT;
    case SDL_SCANCODE_F9: return A_BOND;
    case SDL_SCANCODE_F10: return A_SPLIT;
    case SDL_SCANCODE_F11: return A_SPEED;
    case SDL_SCANCODE_F12: return A_SHOT;
    case SDL_SCANCODE_PAUSE: return A_PAUSE;
    case SDL_SCANCODE_INSERT: return A_SKIP;
    case SDL_SCANCODE_HOME: return A_OUTPUT;
    case SDL_SCANCODE_PAGEUP: return A_CAPS;
    case SDL_SCANCODE_PAGEDOWN: return A_NUM;
    case SDL_SCANCODE_END: return A_SCROLL;
    case SDL_SCANCODE_DELETE: return A_AUTOTYPE;
    case SDL_SCANCODE_SCROLLLOCK: return A_HELP;
    default: return -1;
    }
}

/* ------------------------------------------------------------ PC keyboard */

static int scancode_pos(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_TAB:
    case SDL_SCANCODE_ESCAPE:
    case SDL_SCANCODE_GRAVE: return 0;
    case SDL_SCANCODE_Q: return 1;
    case SDL_SCANCODE_W: return 2;
    case SDL_SCANCODE_E: return 3;
    case SDL_SCANCODE_R: return 4;
    case SDL_SCANCODE_T: return 5;
    case SDL_SCANCODE_Y: return 6;
    case SDL_SCANCODE_U: return 7;
    case SDL_SCANCODE_I: return 8;
    case SDL_SCANCODE_O: return 9;
    case SDL_SCANCODE_P: return 10;
    case SDL_SCANCODE_BACKSPACE:
    case SDL_SCANCODE_LEFTBRACKET: return 11;
    case SDL_SCANCODE_CAPSLOCK: return 12;
    case SDL_SCANCODE_A: return 13;
    case SDL_SCANCODE_S: return 14;
    case SDL_SCANCODE_D: return 15;
    case SDL_SCANCODE_F: return 16;
    case SDL_SCANCODE_G: return 17;
    case SDL_SCANCODE_H: return 18;
    case SDL_SCANCODE_J: return 19;
    case SDL_SCANCODE_K: return 20;
    case SDL_SCANCODE_L: return 21;
    case SDL_SCANCODE_SEMICOLON: return 22;
    case SDL_SCANCODE_APOSTROPHE: return 23;
    case SDL_SCANCODE_LSHIFT: return 24;
    case SDL_SCANCODE_Z: return 25;
    case SDL_SCANCODE_X: return 26;
    case SDL_SCANCODE_C: return 27;
    case SDL_SCANCODE_V: return 28;
    case SDL_SCANCODE_B: return 29;
    case SDL_SCANCODE_N: return 30;
    case SDL_SCANCODE_M: return 31;
    case SDL_SCANCODE_COMMA: return 32;
    case SDL_SCANCODE_PERIOD: return 33;
    case SDL_SCANCODE_SLASH: return 34;
    case SDL_SCANCODE_RSHIFT: return 35;
    case SDL_SCANCODE_LCTRL: return 36;
    case SDL_SCANCODE_LALT: return 37;
    case SDL_SCANCODE_SPACE: return 38;
    case SDL_SCANCODE_RETURN:
    case SDL_SCANCODE_KP_ENTER: return 39;
    case SDL_SCANCODE_RALT: return 40;
    case SDL_SCANCODE_RCTRL:
    case SDL_SCANCODE_APPLICATION: return 41;
    default: return -1;
    }
}

static int8_t sc_held_pos[SDL_NUM_SCANCODES]; /* pos+1 the scancode holds, 0 none */
static int mouse_pos = -1;                    /* key held by the left mouse button */
static bool latched[256];

static void release_keyboard(void) {
    for (int sc = 0; sc < SDL_NUM_SCANCODES; sc++) {
        if (sc_held_pos[sc]) {
            host_key_ref(sc_held_pos[sc] - 1, false);
            sc_held_pos[sc] = 0;
        }
    }
}

static void release_mouse(void) {
    if (mouse_pos >= 0) {
        host_key_ref(mouse_pos, false);
        mouse_pos = -1;
    }
}

/* ------------------------------------------------------------ layout */

#define MARGIN 12
#define LINE_H 17

struct layout {
    int pscale, pw, ph;       /* scaled rotated panel */
    SDL_Rect pbox[2];         /* bezel boxes */
    int info_y;               /* first info line under the panels */
    SDL_Rect mid;             /* status column */
    int kw, kh, kgap, khalf;  /* key geometry */
    int kx0, ky0;
    SDL_Rect keys[256];
    SDL_Rect btn[A_COUNT];
    int footer_y;
};

static struct layout L;
static const int col_stagger[6] = {10, 10, 3, 0, 3, 6}; /* Corne-ish column stagger */

static void layout_compute(void) {
    int pw0 = H.half[0].panel.w > 0 ? H.half[0].panel.w : 160;
    int ph0 = H.half[0].panel.h > 0 ? H.half[0].panel.h : 68;
    int rw, rh;
    panel_rotated_size(pw0, ph0, H.rotation, &rw, &rh);
    L.pscale = (rw > 200) ? 1 : 2;
    L.pw = rw * L.pscale;
    L.ph = rh * L.pscale;
    int bw = L.pw + 32;
    if (bw < 270) {
        bw = 270;
    }
    int bh = L.ph + 32;

    /* keyboard geometry */
    L.kw = 58;
    L.kh = 46;
    L.kgap = 5;
    L.khalf = 56;
    int kb_w = 12 * (L.kw + L.kgap) - L.kgap + L.khalf;
    int kb_h = 4 * (L.kh + L.kgap) + 10 + 10;

    int mid_min = 560;
    win_w = 2 * bw + 2 * MARGIN + 2 * 16 + mid_min;
    if (win_w < kb_w + 2 * MARGIN) {
        win_w = kb_w + 2 * MARGIN;
    }
    int mid_w = win_w - 2 * bw - 2 * MARGIN - 2 * 16;

    L.pbox[0] = (SDL_Rect){MARGIN, MARGIN, bw, bh};
    L.pbox[1] = (SDL_Rect){win_w - MARGIN - bw, MARGIN, bw, bh};
    L.info_y = MARGIN + bh + 8;
    int top_h = L.info_y + 6 * LINE_H;
    L.mid = (SDL_Rect){MARGIN + bw + 16, MARGIN, mid_w, top_h - MARGIN};
    if (L.mid.h < 25 * LINE_H) {
        L.mid.h = 25 * LINE_H;
        top_h = MARGIN + L.mid.h;
    }

    L.kx0 = (win_w - kb_w) / 2;
    L.ky0 = top_h + 16;
    for (int pos = 0; pos < host_num_keys() && pos < 256; pos++) {
        int row = host_key_row(pos), col = host_key_col(pos);
        int x = L.kx0 + col * (L.kw + L.kgap) + (col >= 6 ? L.khalf : 0);
        int y = L.ky0 + row * (L.kh + L.kgap);
        if (row < 3) {
            int c = col < 6 ? col : 11 - col;
            y += col_stagger[c];
        } else {
            y += 14;
        }
        L.keys[pos] = (SDL_Rect){x, y, L.kw, L.kh};
    }

    int by0 = L.ky0 + kb_h + 14;
    const int per_row = 7, bgap = 6, bh_ = 38;
    int bwid = (win_w - 2 * MARGIN - (per_row - 1) * bgap) / per_row;
    for (int a = 0; a < A_COUNT; a++) {
        int r = a / per_row, c = a % per_row;
        L.btn[a] = (SDL_Rect){MARGIN + c * (bwid + bgap), by0 + r * (bh_ + bgap), bwid, bh_};
    }
    int rows = (A_COUNT + per_row - 1) / per_row;
    L.footer_y = by0 + rows * (bh_ + bgap) + 2;
    win_h = L.footer_y + 2 * LINE_H + MARGIN;
}

/* ------------------------------------------------------------ drawing */

static SDL_Texture *panel_tex[2];
static uint8_t *grey_buf;

static void draw_panels(void) {
    for (int i = 0; i < 2; i++) {
        SDL_Rect b = L.pbox[i];
        fill(b.x, b.y, b.w, b.h, C_BEZEL);
        frame_rect(b.x, b.y, b.w, b.h, C_BEZEL_EDGE);
        int px = b.x + (b.w - L.pw) / 2, py = b.y + (b.h - L.ph) / 2;
        struct panel *p = &H.half[i].panel;
        if (p->w <= 0 || p->h <= 0 || !p->model || !panel_tex[i]) {
            text(&F_SMALL, px, py, C_DIM, "(no panel)");
            continue;
        }
        int rw, rh;
        panel_rotated_size(p->w, p->h, H.rotation, &rw, &rh);
        int w = rw * L.pscale, h = rh * L.pscale;
        panel_blit(p, H.t * 1000, H.rotation, L.pscale, grey_buf, w, 0, 0);
        void *pix;
        int pitch;
        if (SDL_LockTexture(panel_tex[i], NULL, &pix, &pitch) == 0) {
            for (int y = 0; y < h; y++) {
                uint8_t *d = (uint8_t *)pix + (size_t)y * pitch;
                const uint8_t *s = grey_buf + (size_t)y * w;
                for (int x = 0; x < w; x++) {
                    d[3 * x] = d[3 * x + 1] = d[3 * x + 2] = s[x];
                }
            }
            SDL_UnlockTexture(panel_tex[i]);
        }
        SDL_Rect dst = {px, py, w, h};
        SDL_RenderCopy(ren, panel_tex[i], NULL, &dst);
        frame_rect(px - 1, py - 1, w + 2, h + 2, (rgba){40, 40, 44, 255});
    }
}

static void draw_half_info(int i) {
    SDL_Rect b = L.pbox[i];
    int x = b.x + 4, y = L.info_y;
    struct half *hf = &H.half[i];
    text(&F_BIG, x, y, C_HEAD, i == 0 ? "CENTRAL (left)" : "PERIPHERAL (right)");
    y += LINE_H + 2;
    int fw_batt = (int)host_query(i, SIM_Q_BATTERY);
    int xx = x + textf(&F_BIG, x, y, C_TEXT, "Battery %3d%%", hf->battery);
    textf(&F_SMALL, xx + 8, y + 3, C_DIM, "(fw sees %d%%)", fw_batt);
    y += LINE_H;
    bool charging = hf->usb == SIM_USB_POWERED || hf->usb == SIM_USB_HID;
    xx = x + textf(&F_BIG, x, y, C_TEXT, "USB %s", host_usb_name(hf->usb));
    if (charging) {
        text(&F_BIG, xx + 8, y, C_GOOD, "charging");
    }
    y += LINE_H;
    bool fw_split = host_query(i, SIM_Q_SPLIT_CONNECTED) != 0;
    xx = x + text(&F_BIG, x, y, C_TEXT, "Split ");
    xx += text(&F_BIG, xx, y, H.split_connected ? C_GOOD : C_BAD,
               H.split_connected ? "link up" : "link down");
    textf(&F_SMALL, xx + 8, y + 3, C_DIM, "(fw: %s)", fw_split ? "connected" : "disconnected");
    y += LINE_H;
    textf(&F_BIG, x, y, C_TEXT, "Display writes %lld",
          (long long)host_query(i, SIM_Q_DISPLAY_WRITES));
    y += LINE_H;
    if (hf->reboots > 0) {
        textf(&F_SMALL, x, y + 2, C_WARN, "reboots: %d", hf->reboots);
    }
}

static const char *yes_no(bool v) { return v ? "yes" : "no"; }

static void draw_status(void) {
    SDL_Rect m = L.mid;
    fill(m.x, m.y, m.w, m.h, C_PANEL_BG);
    int x = m.x + 12, y = m.y + 10;
    int maxc = (m.w - 24) / F_BIG.cw;

    int xx = x + textf(&F_BIG, x, y, C_TEXT, "t = %.3f s   speed %dx", H.t / 1000.0,
                       speeds[speed_idx]);
    if (paused) {
        text(&F_BIG, xx + 16, y, C_WARN, "PAUSED");
    }
    y += LINE_H + 6;

    int layer = (int)host_query(0, SIM_Q_HIGHEST_LAYER);
    xx = x + text(&F_BIG, x, y, C_DIM, "Layer   ");
    xx += textf(&F_BIG, xx, y, C_HEAD, "%d %s", layer, host_layer_name(layer));
    textf(&F_BIG, xx + 16, y, C_DIM, "mask 0x%02llx",
          (unsigned long long)host_query(0, SIM_Q_LAYER_STATE));
    y += LINE_H;

    char mbuf[96];
    host_mods_str((uint8_t)host_query(0, SIM_Q_EXPLICIT_MODS), mbuf, sizeof(mbuf));
    textf(&F_BIG, x, y, C_TEXT, "WPM     %-5lld  mods %s", (long long)host_query(0, SIM_Q_WPM),
          mbuf[0] ? mbuf : "-");
    y += LINE_H + 6;

    int transport = (int)host_query(0, SIM_Q_ENDPOINT_TRANSPORT);
    xx = x + text(&F_BIG, x, y, C_DIM, "Output  ");
    text(&F_BIG, xx, y, C_HEAD, transport == 0 ? "USB" : transport == 1 ? "BLE" : "none");
    y += LINE_H;
    int prof = active_profile();
    bool conn = host_query(0, SIM_Q_PROFILE_CONNECTED) != 0;
    bool bonded = host_query(0, SIM_Q_PROFILE_BONDED) != 0;
    bool present = prof >= 0 && prof < 5 && H.ble_host_present[prof];
    xx = x + textf(&F_BIG, x, y, C_DIM, "BLE     ");
    xx += textf(&F_BIG, xx, y, C_TEXT, "profile %d  ", prof);
    xx += text(&F_BIG, xx, y, conn ? C_GOOD : C_BAD, conn ? "connected" : "not connected");
    textf(&F_BIG, xx, y, C_TEXT, "  bonded %s", yes_no(bonded));
    y += LINE_H;
    textf(&F_BIG, x, y, C_DIM, "        host present for profile %d: %s", prof, yes_no(present));
    y += LINE_H;
    xx = x + text(&F_BIG, x, y, C_DIM, "        hosts: ");
    for (int i = 0; i < 5; i++) {
        char c[4] = {(char)('0' + i), H.ble_host_present[i] ? '+' : '-', ' ', 0};
        xx += text(&F_BIG, xx, y, i == prof ? C_HEAD : C_DIM, c);
    }
    y += LINE_H + 6;

    int ind = (int)host_query(0, SIM_Q_HID_INDICATORS);
    xx = x + text(&F_BIG, x, y, C_DIM, "LEDs    host ");
    static const char *ln[3] = {"NUM", "CAPS", "SCRL"};
    for (int b = 0; b < 3; b++) {
        xx += text(&F_BIG, xx, y, (H.leds >> b) & 1 ? C_GOOD : (rgba){80, 86, 98, 255}, ln[b]);
        xx += F_BIG.cw;
    }
    xx += text(&F_BIG, xx, y, C_DIM, " fw ");
    for (int b = 0; b < 3; b++) {
        xx += text(&F_BIG, xx, y, (ind >> b) & 1 ? C_GOOD : (rgba){80, 86, 98, 255}, ln[b]);
        xx += F_BIG.cw;
    }
    y += LINE_H;

    int act = (int)host_query(0, SIM_Q_ACTIVITY_STATE);
    static const char *an[3] = {"active", "idle", "sleep"};
    xx = x + text(&F_BIG, x, y, C_DIM, "Activ.  ");
    text(&F_BIG, xx, y, act == 0 ? C_GOOD : C_WARN, (act >= 0 && act < 3) ? an[act] : "?");
    y += LINE_H;
    textf(&F_BIG, x, y, C_TEXT, "Panel   %s, rot %d", H.panel_model ? H.panel_model->name : "?",
          H.rotation);
    y += LINE_H;
    if (H.autotype_wpm > 0) {
        textf(&F_BIG, x, y, C_TEXT, "Autotype %d WPM", H.autotype_wpm);
    } else {
        text(&F_BIG, x, y, C_DIM, "Autotype off");
    }
    textf(&F_SMALL, x + 22 * F_BIG.cw, y + 3, C_DIM, "HID reports %lld   LVGL heap %lld/%lld",
          H.hid_reports, (long long)host_query(0, SIM_Q_LVGL_HEAP_USED),
          (long long)host_query(0, SIM_Q_LVGL_HEAP_PEAK));
    y += LINE_H + 6;

    /* text the computer received: last 80 chars, wrapped */
    text(&F_BIG, x, y, C_DIM, "Computer received:");
    y += LINE_H;
    char tail[81];
    int n = H.typed_len < 80 ? H.typed_len : 80;
    const char *src = H.typed + (H.typed_len - n);
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)src[i];
        tail[i] = c == '\n' ? '|' : c == '\t' ? ' ' : (c < 32 || c > 126) ? '.' : (char)c;
    }
    tail[n] = 0;
    int per_line = maxc - 2 < 40 ? maxc - 2 : 40;
    if (per_line < 10) {
        per_line = 10;
    }
    fill(x - 2, y - 1, per_line * F_BIG.cw + 4 + F_BIG.cw, 2 * LINE_H + 4, (rgba){18, 20, 24, 255});
    for (int line = 0, off = 0; line < 2 && off < n; line++, off += per_line) {
        int k = n - off < per_line ? n - off : per_line;
        text_n(&F_BIG, x, y + line * LINE_H, C_GOOD, tail + off, k);
    }
    /* caret */
    int cl = n == 0 ? 0 : (n - 1) / per_line, cc = n == 0 ? 0 : n - cl * per_line;
    if (cl < 2 && (SDL_GetTicks() / 500) % 2) {
        fill(x + cc * F_BIG.cw, y + cl * LINE_H + 2, 2, F_BIG.ch - 2, C_GOOD);
    }
    y += 2 * LINE_H + 10;

    text(&F_SMALL, x, y, C_DIM,
         "Battery changes show at battery.c's next 60 s sample: use Skip +30 s.");
    y += F_SMALL.ch + 2;
    text(&F_SMALL, x, y, C_DIM, "Idle after 30 s without input (Skip +30 s to reach it).");
}

static void split_label(const char *label, char *l1, char *l2, size_t n) {
    const char *slash = strchr(label, '/');
    l1[0] = l2[0] = 0;
    if (slash) {
        size_t k = (size_t)(slash - label);
        if (k >= n) {
            k = n - 1;
        }
        memcpy(l1, label, k);
        l1[k] = 0;
        /* "LEFT_SHIFT" -> "L_SHIFT" so hold-tap halves fit the key */
        const char *h = slash + 1;
        if (!strncmp(h, "LEFT_", 5) || !strncmp(h, "RIGHT_", 6)) {
            snprintf(l2, n, "%c_%s", h[0], strchr(h, '_') + 1);
        } else {
            snprintf(l2, n, "%s", h);
        }
        return;
    }
    snprintf(l1, n, "%s", label);
    const char *sp = strchr(label, ' ');
    if (sp && (int)strlen(label) * F_BIG.cw > L.kw - 6) {
        size_t k = (size_t)(sp - label);
        if (k >= n) {
            k = n - 1;
        }
        l1[k] = 0;
        snprintf(l2, n, "%s", sp + 1);
    }
}

static void draw_centered(int cx, int y, int maxw, rgba c, const char *s) {
    const struct font *f = &F_BIG;
    int len = (int)strlen(s);
    if (len * f->cw > maxw) {
        f = &F_SMALL;
    }
    int maxc = maxw / f->cw;
    if (len > maxc) {
        len = maxc;
    }
    text_n(f, cx - len * f->cw / 2, y + (F_BIG.ch - f->ch) / 2, c, s, len);
}

static void draw_keyboard(int hover_pos) {
    int layer = (int)host_query(0, SIM_Q_HIGHEST_LAYER);
    for (int pos = 0; pos < host_num_keys() && pos < 256; pos++) {
        SDL_Rect r = L.keys[pos];
        bool down = H.key_down[pos];
        rgba bg = latched[pos] ? (down ? C_KEY_LATCH_DOWN : C_KEY_LATCH) : down ? C_KEY_DOWN : C_KEY;
        if (pos == hover_pos && !down && !latched[pos]) {
            bg = (rgba){54, 59, 70, 255};
        }
        fill(r.x, r.y, r.w, r.h, bg);
        frame_rect(r.x, r.y, r.w, r.h, down ? (rgba){150, 190, 255, 255} : C_KEY_EDGE);
        char l1[64], l2[64];
        split_label(host_key_label(layer, pos), l1, l2, sizeof(l1));
        rgba tc = down || latched[pos] ? (rgba){255, 255, 255, 255} : C_TEXT;
        int cx = r.x + r.w / 2;
        if (l2[0]) {
            draw_centered(cx, r.y + 6, r.w - 4, tc, l1);
            const struct font *f = &F_SMALL;
            int len = (int)strlen(l2), maxc = (r.w - 4) / f->cw;
            if (len > maxc) {
                len = maxc;
            }
            text_n(f, cx - len * f->cw / 2, r.y + r.h - f->ch - 6,
                   down || latched[pos] ? (rgba){230, 230, 240, 255} : C_DIM, l2, len);
        } else {
            draw_centered(cx, r.y + (r.h - F_BIG.ch) / 2, r.w - 4, tc, l1);
        }
        char num[8];
        snprintf(num, sizeof(num), "%d", pos);
        text(&F_SMALL, r.x + 2, r.y + 1, (rgba){90, 96, 110, 255}, num);
    }
    /* half captions */
    int ty = L.ky0 - F_SMALL.ch - 2;
    text(&F_SMALL, L.kx0, ty, C_DIM, "left half -> central");
    const char *rc = "right half -> peripheral";
    text(&F_SMALL, L.kx0 + 12 * (L.kw + L.kgap) - L.kgap + L.khalf - text_w(&F_SMALL, rc), ty,
         C_DIM, rc);
    char lb[96];
    snprintf(lb, sizeof(lb), "layer %d: %s", layer, host_layer_name(layer));
    text(&F_SMALL, win_w / 2 - text_w(&F_SMALL, lb) / 2, ty, C_HEAD, lb);
}

static void draw_buttons(int mx, int my) {
    for (int a = 0; a < A_COUNT; a++) {
        SDL_Rect r = L.btn[a];
        bool hot = in_rect(mx, my, &r);
        bool on = action_on(a);
        fill(r.x, r.y, r.w, r.h, on ? C_BTN_ON : hot ? C_BTN_HOT : C_BTN);
        frame_rect(r.x, r.y, r.w, r.h, hot ? (rgba){110, 120, 140, 255} : C_KEY_EDGE);
        char lbl[64];
        action_label(a, lbl, sizeof(lbl));
        int maxc = (r.w - 8) / F_BIG.cw;
        text_n(&F_BIG, r.x + 6, r.y + 4, C_TEXT, lbl, maxc);
        if (action_hint[a][0]) {
            text(&F_SMALL, r.x + r.w - 4 - text_w(&F_SMALL, action_hint[a]),
                 r.y + r.h - F_SMALL.ch - 3, C_DIM, action_hint[a]);
        }
    }
    int y = L.footer_y;
    text(&F_SMALL, MARGIN, y, C_DIM,
         "Left click a key = press while held.  Right click = latch/unlatch hold.  "
         "PC keyboard maps onto the Corne.  ScrollLock = help.");
    if (toast[0] && SDL_GetTicks() < toast_until) {
        text(&F_BIG, MARGIN, y + LINE_H, C_WARN, toast);
    }
}

static const char *help_lines[] = {
    "KEYBOARD (scancodes, layout independent)",
    "  row 0: Tab/Esc/`  Q W E R T  |  Y U I O P  Backspace/[",
    "  row 1: CapsLock   A S D F G  |  H J K L ;  '",
    "  row 2: LShift     Z X C V B  |  N M , . /  RShift",
    "  thumbs: LCtrl LAlt Space  |  Enter RAlt RCtrl/Menu",
    "",
    "MOUSE",
    "  left click key: press while held   right click key: latch / unlatch",
    "",
    "CONTROLS",
    "  F1/F2  central battery -/+10     F3/F4  peripheral battery -/+10",
    "  F5     central USB cycle         F6     peripheral USB cycle",
    "         (none -> powered -> hid; powered/hid = charging)",
    "  F7     next BLE profile          F8     toggle host present (active profile)",
    "  F9     toggle bond               F10    toggle split link",
    "  Home   output USB/BLE            Delete auto-type off/40/80/120 WPM",
    "  PgUp   host Caps Lock            PgDn   host Num Lock      End  host Scroll Lock",
    "  F11    speed 1/2/5/10/30x        Pause  pause / resume",
    "  Insert skip +30 s (idle timeout is 30 s)",
    "  F12    screenshot (PNG)          ScrollLock  this help",
    "",
    "Battery: firmware battery.c samples every 60 s, so changes appear at the",
    "next sample (use Skip).  Close the window to quit.",
};

static void draw_help(void) {
    int n = (int)(sizeof(help_lines) / sizeof(help_lines[0]));
    int w = 84 * F_BIG.cw + 40, h = n * LINE_H + 40;
    if (w > win_w - 40) {
        w = win_w - 40;
    }
    int x = (win_w - w) / 2, y = (win_h - h) / 2;
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_BLEND);
    fill(0, 0, win_w, win_h, (rgba){0, 0, 0, 150});
    SDL_SetRenderDrawBlendMode(ren, SDL_BLENDMODE_NONE);
    fill(x, y, w, h, (rgba){30, 33, 40, 255});
    frame_rect(x, y, w, h, C_HEAD);
    for (int i = 0; i < n; i++) {
        const char *s = help_lines[i];
        text(&F_BIG, x + 20, y + 20 + i * LINE_H, s[0] != ' ' ? C_HEAD : C_TEXT, s);
    }
}

/* ------------------------------------------------------------ main loop */

static int key_at(int mx, int my) {
    for (int pos = 0; pos < host_num_keys() && pos < 256; pos++) {
        if (in_rect(mx, my, &L.keys[pos])) {
            return pos;
        }
    }
    return -1;
}

static int button_at(int mx, int my) {
    for (int a = 0; a < A_COUNT; a++) {
        if (in_rect(mx, my, &L.btn[a])) {
            return a;
        }
    }
    return -1;
}

static void update_title(void) {
    static int last_layer = -2;
    int layer = (int)host_query(0, SIM_Q_HIGHEST_LAYER);
    if (layer != last_layer) {
        char t[160];
        snprintf(t, sizeof(t), "ZMK nice!view sim - %s", host_layer_name(layer));
        SDL_SetWindowTitle(win, t);
        last_layer = layer;
    }
}

static void handle_event(const SDL_Event *e) {
    switch (e->type) {
    case SDL_QUIT:
        H.quit = true;
        break;
    case SDL_WINDOWEVENT:
        if (e->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            release_keyboard();
            release_mouse();
        } else if (e->window.event == SDL_WINDOWEVENT_CLOSE) {
            H.quit = true;
        }
        break;
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        bool down = e->type == SDL_KEYDOWN;
        SDL_Scancode sc = e->key.keysym.scancode;
        if (sc < 0 || sc >= SDL_NUM_SCANCODES) {
            break;
        }
        int a = scancode_action(sc);
        if (a >= 0) {
            if (down && !e->key.repeat) {
                do_action(a);
            }
            break;
        }
        if (e->key.repeat) {
            break;
        }
        if (down) {
            int pos = scancode_pos(sc);
            if (pos >= 0 && pos < host_num_keys() && !sc_held_pos[sc]) {
                sc_held_pos[sc] = (int8_t)(pos + 1);
                host_key_ref(pos, true);
            }
        } else if (sc_held_pos[sc]) {
            host_key_ref(sc_held_pos[sc] - 1, false);
            sc_held_pos[sc] = 0;
        }
        break;
    }
    case SDL_MOUSEBUTTONDOWN: {
        int mx = e->button.x, my = e->button.y;
        if (show_help && e->button.button == SDL_BUTTON_LEFT) {
            int b = button_at(mx, my);
            if (b != A_HELP) {
                show_help = false;
                break;
            }
        }
        int pos = key_at(mx, my);
        if (e->button.button == SDL_BUTTON_LEFT) {
            if (pos >= 0) {
                release_mouse();
                mouse_pos = pos;
                host_key_ref(pos, true);
            } else {
                int b = button_at(mx, my);
                if (b >= 0) {
                    do_action(b);
                }
            }
        } else if (e->button.button == SDL_BUTTON_RIGHT && pos >= 0) {
            latched[pos] = !latched[pos];
            host_key_ref(pos, latched[pos]);
        }
        break;
    }
    case SDL_MOUSEBUTTONUP:
        if (e->button.button == SDL_BUTTON_LEFT) {
            release_mouse();
        }
        break;
    }
}

static void ui_cleanup(void) {
    release_keyboard();
    release_mouse();
    for (int i = 0; i < 2; i++) {
        if (panel_tex[i]) {
            SDL_DestroyTexture(panel_tex[i]);
            panel_tex[i] = NULL;
        }
    }
    if (F_BIG.tex) {
        SDL_DestroyTexture(F_BIG.tex);
        F_BIG.tex = NULL;
    }
    if (F_SMALL.tex) {
        SDL_DestroyTexture(F_SMALL.tex);
        F_SMALL.tex = NULL;
    }
    free(grey_buf);
    grey_buf = NULL;
    if (ren) {
        SDL_DestroyRenderer(ren);
        ren = NULL;
    }
    if (win) {
        SDL_DestroyWindow(win);
        win = NULL;
    }
    SDL_Quit();
}

int ui_run(void) {
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 2;
    }
    layout_compute();
    win = SDL_CreateWindow("ZMK nice!view sim", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                           win_w, win_h, SDL_WINDOW_SHOWN);
    if (!win) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 2;
    }
    bool vsync = true;
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        vsync = false;
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!ren) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        ui_cleanup();
        return 2;
    }
    if (!font_build(&F_BIG, 15, false) || !font_build(&F_SMALL, 11, false)) {
        fprintf(stderr, "warning: GDI font atlas could not be built; text will be missing\n");
    }

    /* panel textures */
    size_t maxpix = 0;
    for (int i = 0; i < 2; i++) {
        struct panel *p = &H.half[i].panel;
        if (p->w <= 0 || p->h <= 0) {
            continue;
        }
        int rw, rh;
        panel_rotated_size(p->w, p->h, H.rotation, &rw, &rh);
        int w = rw * L.pscale, h = rh * L.pscale;
        if ((size_t)w * h > maxpix) {
            maxpix = (size_t)w * h;
        }
        panel_tex[i] =
            SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB24, SDL_TEXTUREACCESS_STREAMING, w, h);
    }
    grey_buf = calloc(maxpix ? maxpix : 1, 1);

    sim_target = (double)H.t;
    uint64_t freq = SDL_GetPerformanceFrequency();
    uint64_t last = SDL_GetPerformanceCounter();

    while (!H.quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            handle_event(&e);
        }
        if (H.quit) {
            break;
        }

        host_watch_poll();

        uint64_t now = SDL_GetPerformanceCounter();
        double dt_ms = (double)(now - last) * 1000.0 / (double)freq;
        last = now;
        if (dt_ms > 250) {
            dt_ms = 250; /* window dragged / debugger: don't jump */
        }
        if (sim_target < (double)H.t) {
            sim_target = (double)H.t;
        }
        if (!paused) {
            sim_target += dt_ms * speeds[speed_idx];
            if (sim_target > (double)H.t + MAX_CATCHUP_MS) {
                sim_target = (double)H.t + MAX_CATCHUP_MS;
            }
            host_step_to((int64_t)sim_target);
        }

        update_title();
        int mx, my;
        SDL_GetMouseState(&mx, &my);

        set_col(C_BG);
        SDL_RenderClear(ren);
        draw_panels();
        draw_half_info(0);
        draw_half_info(1);
        draw_status();
        draw_keyboard(key_at(mx, my));
        draw_buttons(mx, my);
        if (show_help) {
            draw_help();
        }
        SDL_RenderPresent(ren);

        if (!vsync) {
            uint64_t end = SDL_GetPerformanceCounter();
            double spent = (double)(end - now) * 1000.0 / (double)freq;
            if (spent < 16) {
                SDL_Delay((uint32_t)(16 - spent));
            }
        }
    }

    ui_cleanup();
    return H.expect_failed ? 1 : 0;
}
