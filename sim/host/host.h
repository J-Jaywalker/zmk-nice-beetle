/*
 * Simulator host (sim.exe): shared declarations.
 *
 *   main.c    command line, MCU DLL loading, lock-step clock, split link,
 *             key routing, USB/BLE host + OS model, headless run loop
 *   panel.c   display post-processing stage (ideal 1-bit panel or Sharp
 *             memory-LCD model) + rotation
 *   script.c  scripted input (text or JSON) and `expect` checks
 *   image.c   PNG / GIF writers (no external libraries)
 *   ui.c      SDL2 window: panels, on-screen Corne, controls, PC keyboard
 */
#ifndef SIM_HOST_H
#define SIM_HOST_H

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include "sim_api.h"

#define HOST_MAX_FB (400 * 400)

/* ------------------------------------------------------------ panel.c */

/* A display post-processing model.  It receives every write the firmware's
 * display driver makes and produces the grey image (0 = black, 255 = white)
 * the viewer sees at time t, in panel (unrotated) coordinates. */
struct panel;
struct panel_model {
    const char *name;
    /* start_us: host time (us) the write's SPI transfer starts; xfer_us: its length */
    void (*on_write)(struct panel *p, int64_t start_us, const uint8_t *fb, int y0, int y1,
                     int xfer_us);
    void (*render)(struct panel *p, int64_t t_us, uint8_t *grey);
    /* still changing on its own at t (transfer in flight, pixels settling) */
    bool (*busy)(struct panel *p, int64_t t_us);
};

struct panel {
    const struct panel_model *model;
    int w, h;              /* panel (driver) resolution */
    uint8_t fb[HOST_MAX_FB]; /* latest driver framebuffer, 1 = white */
    bool valid;            /* at least one write happened */
    void *model_state;
};

extern const struct panel_model panel_model_ideal, panel_model_memlcd;
extern int memlcd_response_ms;
const struct panel_model *panel_model_by_name(const char *name);
void panel_init(struct panel *p, int w, int h, const struct panel_model *m);
/* rotation is clockwise degrees applied to the panel image for viewing */
void panel_rotated_size(int w, int h, int rot, int *ow, int *oh);
/* render the panel (through its model) rotated + scaled into dst (grey) */
void panel_blit(struct panel *p, int64_t t_us, int rot, int scale, uint8_t *dst, int dst_w, int dx,
                int dy);

/* ------------------------------------------------------------- image.c */

int png_write_grey(const char *path, const uint8_t *img, int w, int h);
int png_write_rgb(const char *path, const uint8_t *rgb, int w, int h);

struct gif_writer;
struct gif_writer *gif_open(const char *path, int w, int h);
/* add a frame shown for delay_cs centiseconds */
void gif_frame(struct gif_writer *g, const uint8_t *grey, int delay_cs);
void gif_close(struct gif_writer *g);

/* -------------------------------------------------------------- main.c */

enum { HALF_CENTRAL = 0, HALF_PERIPHERAL = 1 };

struct half {
    int role;
    const char *name;
    char dll_path[1024];
    void *module;
    const struct sim_mcu_api *api;
    struct sim_host_callbacks cb;
    int64_t t_offset; /* host time at which this MCU booted */
    struct panel panel;
    bool dirty;       /* display changed since last composite */
    /* host-side hardware state (re-applied after a reboot) */
    int battery;
    int usb;
    int reboots;
    bool halted;      /* entered the bootloader: firmware no longer runs */
    long long packets_tx;
    /* the DLL is loaded from a shadow copy so the build can overwrite dll_path */
    char shadow_path[1100];
    unsigned long long loaded_stamp;  /* mtime of dll_path when it was copied */
    unsigned long long pending_stamp; /* --watch: changed mtime waiting to settle */
    unsigned long long pending_since; /* GetTickCount64() when pending_stamp was seen */
    int reloads;
};

struct split_packet {
    int64_t deliver_at;
    int to;
    size_t len;
    uint8_t data[64];
};

struct host {
    struct half half[2];
    int64_t t; /* host (wall) time in ms */
    bool split_connected;
    int split_latency_ms;
    struct split_packet q[256];
    int qn;

    /* computer model (what a PC on the other end of USB/BLE does) */
    uint8_t leds;          /* bit0 num, bit1 caps, bit2 scroll (HID LED usage order) */
    bool leds_dirty;
    uint8_t prev_keys[32];
    int prev_nkeys;
    uint8_t mods;
    char typed[4096];
    int typed_len;
    long long hid_reports;
    char last_consumer[48];
    bool ble_host_present[5];

    /* user model */
    bool key_down[256];   /* physical key held (any source) */
    int key_refs[256];    /* number of sources holding it (PC key, mouse, script) */

    /* view */
    int rotation;         /* clockwise degrees */
    int scale;            /* headless image scale */
    const struct panel_model *panel_model; /* --panel ideal|memlcd */
    int log_level;
    FILE *log_file;
    bool quiet;
    char shot_dir[512];   /* where UI screenshots go (created on demand) */
    int shot_count;

    /* auto typer */
    int autotype_wpm;
    int64_t autotype_next;
    int autotype_idx;
    int autotype_pending_release; /* position or -1 */
    int64_t autotype_release_at;

    /* test result */
    int expect_failed, expect_passed;
    bool quit;

    /* --watch: reload a half when its DLL is rebuilt (host_watch_poll) */
    bool watch;
};

extern struct host H;

/* --watch: call regularly (cheap, self-throttled); reloads rebuilt MCU DLLs */
void host_watch_poll(void);

/* Contract notes:
 *  - All functions run on the single host thread; never call them from inside
 *    an MCU callback.
 *  - host_key_ref(): every input source (PC keyboard, mouse, script, auto
 *    typer) uses this; the physical switch closes on the first press and opens
 *    when the last source releases.  Routing: left-half keys go to the central
 *    MCU, right-half keys to the peripheral MCU (which forwards them over the
 *    split link like the real firmware).  H.key_down[pos] mirrors the switch.
 *  - host_step_to(t): advances host time H.t to t in 1 ms lock-step: per ms it
 *    runs due script commands, the auto typer, both MCUs, split packet relay
 *    and the computer (LED) model.  t <= H.t is a no-op.
 *  - Panels: H.half[i].panel, drawn with panel_blit(&p, H.t * 1000, H.rotation,
 *    scale, ...).  H.half[i].dirty is set by display writes (consumers clear it).
 *  - host_save_screenshot(path): PNG of host_composite(H.scale).
 */
void host_key(int pos, bool pressed);      /* raw switch change (bypasses ref counting) */
void host_key_ref(int pos, bool pressed);  /* ref-counted: use this from UI / script */
void host_step_to(int64_t t);              /* advance both MCUs to host time t (1 ms steps) */
const char *host_usb_name(int state);      /* "none" / "powered" / "hid" */
void host_set_battery(int half, int pct);
void host_set_usb(int half, int state);
void host_set_split(bool connected);
void host_set_leds(uint8_t leds);
void host_ble_select(int idx);
void host_ble_host_present(int idx, bool present);
void host_ble_bond(int idx, bool bonded);
int host_select_output(int transport);
int64_t host_query(int half, int what);
const char *host_layer_name(int idx);
int host_num_keys(void);
int host_num_layers(void);
int host_key_half(int pos);
int host_key_row(int pos);
int host_key_col(int pos);
const char *host_key_label(int layer, int pos);
/* composite of both panels as seen by the viewer (grey); returns size */
void host_composite_size(int scale, int *w, int *h);
void host_composite(int scale, uint8_t *dst);
int host_save_screenshot(const char *path);
void host_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *host_mods_str(uint8_t mods, char *buf, size_t n);
void host_autotype_set(int wpm);

/* ------------------------------------------------------------ script.c */

int script_load(const char *path);
/* run all script commands due at or before t; returns false when the script
 * asked to stop ("end") */
void script_run_due(int64_t t);
bool script_done(void);
int64_t script_end_time(void);
int script_exec_line(const char *line, int64_t t, const char *where);

/* -------------------------------------------------------------- ui.c */

int ui_run(void);

#endif
