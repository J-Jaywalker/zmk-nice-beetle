/*
 * sim.exe - host side of the ZMK split-keyboard status screen simulator.
 *
 * Loads the two simulated MCUs (central.dll = left half, peripheral.dll =
 * right half), runs them in 1 ms lock-step, relays split-link packets between
 * them, routes key presses to the half that owns the switch, plays the role of
 * the computer (USB/BLE HID host: receives reports, keeps caps/num/scroll lock
 * state and sends the LED report back), and either opens the SDL window
 * (ui.c) or runs headless (scripted, PNG/GIF output).
 */
#include <windows.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>
#include "host.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-const-variable"
#pragma GCC diagnostic ignored "-Wunused-variable"
#include "sim_keymap_info.h"
#include "sim_board_info.h"
#pragma GCC diagnostic pop

struct host H;

static struct sim_init_params base_params;
static int file_log_level = 4;

/* ------------------------------------------------------------ logging */

static void vlog_line(int level, char tag, const char *fmt, va_list ap) {
    char msg[1024];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    char line[1200];
    snprintf(line, sizeof(line), "[%8.3f %c] %s\n", H.t / 1000.0, tag, msg);
    if (level <= H.log_level && !H.quiet) {
        fputs(line, stdout);
        fflush(stdout);
    }
    if (H.log_file && level <= file_log_level) {
        fputs(line, H.log_file);
    }
}

void host_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog_line(1, 'H', fmt, ap); /* host messages always shown */
    va_end(ap);
}

static void host_logf(int level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog_line(level, 'H', fmt, ap);
    va_end(ap);
}

static void cb_log(void *ctx, int role, int level, const char *msg) {
    struct half *h = ctx;
    static const char *lv[] = {"", "ERR", "WRN", "INF", "DBG"};
    va_list dummy;
    (void)dummy;
    char buf[1100];
    snprintf(buf, sizeof(buf), "%s %s", level >= 1 && level <= 4 ? lv[level] : "?", msg);
    /* route through vlog_line with the half's tag */
    char tag = h->role == HALF_CENTRAL ? 'C' : 'P';
    char line[1200];
    snprintf(line, sizeof(line), "[%8.3f %c] %s\n", H.t / 1000.0, tag, buf);
    if (level <= H.log_level && !H.quiet) {
        fputs(line, stdout);
        fflush(stdout);
    }
    if (H.log_file && level <= file_log_level) {
        fputs(line, H.log_file);
    }
}

/* ------------------------------------------------------ keymap helpers */

int host_num_keys(void) { return SIM_NUM_KEYS; }
int host_num_layers(void) { return SIM_NUM_LAYERS; }
int host_key_half(int pos) { return pos >= 0 && pos < SIM_NUM_KEYS ? sim_key_pos[pos].half : 0; }
int host_key_row(int pos) { return pos >= 0 && pos < SIM_NUM_KEYS ? sim_key_pos[pos].row : 0; }
int host_key_col(int pos) { return pos >= 0 && pos < SIM_NUM_KEYS ? sim_key_pos[pos].col : 0; }
const char *host_key_label(int layer, int pos) {
    if (layer < 0 || layer >= SIM_NUM_LAYERS || pos < 0 || pos >= SIM_NUM_KEYS) {
        return "";
    }
    return sim_layer_labels[layer][pos] ? sim_layer_labels[layer][pos] : "";
}
const char *host_layer_name(int idx) {
    if (idx < 0 || idx >= SIM_NUM_LAYERS) {
        return "?";
    }
    return sim_layer_names[idx] ? sim_layer_names[idx] : "?";
}
const char *host_usb_name(int s) {
    return s == SIM_USB_HID ? "hid" : s == SIM_USB_POWERED ? "powered" : "none";
}

const char *host_mods_str(uint8_t m, char *buf, size_t n) {
    static const char *names[8] = {"LCTL", "LSFT", "LALT", "LGUI", "RCTL", "RSFT", "RALT", "RGUI"};
    size_t k = 0;
    buf[0] = 0;
    for (int i = 0; i < 8; i++) {
        if (m & (1 << i)) {
            k += snprintf(buf + k, k < n ? n - k : 0, "%s%s", k ? "+" : "", names[i]);
        }
    }
    if (!k) {
        snprintf(buf, n, "-");
    }
    return buf;
}

/* ---------------------------------------------------------- MCU glue */

static void cb_display(void *ctx, int role, int64_t start_us, int y0, int y1, const uint8_t *fb,
                       int w, int h, int xfer_us) {
    struct half *hf = ctx;
    if (hf->panel.w != w || hf->panel.h != h) {
        panel_init(&hf->panel, w, h, H.panel_model);
    }
    hf->panel.valid = true;
    hf->panel.model->on_write(&hf->panel, start_us + hf->t_offset * 1000, fb, y0, y1, xfer_us);
    hf->dirty = true;
}

static void os_report(const uint8_t *r, size_t len);

static void cb_hid(void *ctx, int transport, uint8_t page, const uint8_t *r, size_t len) {
    H.hid_reports++;
    if (page == 0x07) {
        os_report(r, len);
    } else if (page == 0x0C) {
        unsigned u = 0;
        for (size_t i = 0; i + 1 < len; i += 2) {
            u = r[i] | (r[i + 1] << 8);
            if (u) {
                break;
            }
        }
        if (u) {
            snprintf(H.last_consumer, sizeof(H.last_consumer), "consumer 0x%03x", u);
            host_logf(3, "computer: %s via %s", H.last_consumer, transport ? "BLE" : "USB");
        }
    }
}

static void cb_split(void *ctx, int role, const void *data, size_t len) {
    struct half *hf = ctx;
    if (H.qn >= (int)(sizeof(H.q) / sizeof(H.q[0])) || len > sizeof(H.q[0].data)) {
        host_logf(2, "split: packet dropped (queue full or too big: %u bytes)", (unsigned)len);
        return;
    }
    struct split_packet *p = &H.q[H.qn++];
    p->deliver_at = H.t + H.split_latency_ms;
    p->to = hf->role == HALF_CENTRAL ? HALF_PERIPHERAL : HALF_CENTRAL;
    p->len = len;
    memcpy(p->data, data, len);
    hf->packets_tx++;
}

static char exe_dir[900];

/* mtime of a file as a single number (0 if missing) */
static unsigned long long file_stamp(const char *path, unsigned long long *size) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) {
        return 0;
    }
    if (size) {
        *size = ((unsigned long long)fa.nFileSizeHigh << 32) | fa.nFileSizeLow;
    }
    return ((unsigned long long)fa.ftLastWriteTime.dwHighDateTime << 32) |
           fa.ftLastWriteTime.dwLowDateTime;
}

/* Windows keeps a loaded DLL locked, so each half runs from a private copy
 * (<exe dir>\shadow\<name>.<pid>.<n>.dll): `make sim-build` can overwrite
 * central.dll / peripheral.dll while the simulator runs, and --watch reloads
 * them.  Returns 0 and fills module/api, or -1 (nothing changed in *hf). */
static int load_half_from(struct half *hf, char *err, size_t errlen) {
    static int counter;
    char shadow_dir[1000], shadow[1100];
    snprintf(shadow_dir, sizeof(shadow_dir), "%s\\shadow", exe_dir);
    CreateDirectoryA(shadow_dir, NULL);
    const char *base = strrchr(hf->dll_path, '\\');
    const char *base2 = strrchr(hf->dll_path, '/');
    base = base2 > base ? base2 : base;
    base = base ? base + 1 : hf->dll_path;
    snprintf(shadow, sizeof(shadow), "%s\\%.*s.%lu.%d.dll", shadow_dir,
             (int)(strlen(base) > 4 ? strlen(base) - 4 : strlen(base)), base, GetCurrentProcessId(),
             ++counter);
    unsigned long long stamp = file_stamp(hf->dll_path, NULL);
    if (!CopyFileA(hf->dll_path, shadow, FALSE)) {
        snprintf(err, errlen, "cannot copy %s (error %lu)", hf->dll_path, GetLastError());
        return -1;
    }
    HMODULE m = LoadLibraryA(shadow);
    if (!m) {
        snprintf(err, errlen, "cannot load %s (error %lu)", hf->dll_path, GetLastError());
        DeleteFileA(shadow);
        return -1;
    }
    sim_mcu_get_api_fn get = (sim_mcu_get_api_fn)GetProcAddress(m, "sim_mcu_get_api");
    const struct sim_mcu_api *api = get ? get() : NULL;
    if (!api) {
        snprintf(err, errlen, "%s has no sim_mcu_get_api", hf->dll_path);
    } else if (api->version != SIM_API_VERSION) {
        snprintf(err, errlen, "%s has API version %d, sim.exe expects %d - rebuild (make sim-build) "
                 "and restart the simulator", hf->dll_path, api->version, SIM_API_VERSION);
        api = NULL;
    } else if (api->role != hf->role) {
        snprintf(err, errlen, "%s reports the wrong role", hf->dll_path);
        api = NULL;
    }
    if (!api) {
        FreeLibrary(m);
        DeleteFileA(shadow);
        return -1;
    }
    hf->module = m;
    hf->api = api;
    hf->loaded_stamp = stamp;
    snprintf(hf->shadow_path, sizeof(hf->shadow_path), "%s", shadow);
    return 0;
}

static int load_half(struct half *hf) {
    char err[1400];
    if (load_half_from(hf, err, sizeof(err)) != 0) {
        fprintf(stderr, "sim: %s\n", err);
        return -1;
    }
    return 0;
}

/* remove shadow copies left behind by earlier runs (in-use ones stay) */
static void clean_shadow_dir(void) {
    char pat[1000], f[1100];
    snprintf(pat, sizeof(pat), "%s\\shadow\\*.dll", exe_dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        snprintf(f, sizeof(f), "%s\\shadow\\%s", exe_dir, fd.cFileName);
        DeleteFileA(f);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

static void boot_half(struct half *hf) {
    hf->cb = (struct sim_host_callbacks){hf, cb_log, cb_display, cb_hid, cb_split};
    struct sim_init_params p = base_params;
    p.battery_level = (uint8_t)hf->battery;
    p.usb_state = hf->usb;
    p.split_connected = H.split_connected;
    hf->t_offset = H.t;
    panel_init(&hf->panel, SIM_DISPLAY_WIDTH, SIM_DISPLAY_HEIGHT, H.panel_model);
    hf->dirty = true;
    hf->api->init(&hf->cb, &p);
    if (hf->role == HALF_CENTRAL) {
        /* fake BLE boots with profile 0 bonded + its host present; re-apply the
         * rest of the computer model (bonds of other profiles are not
         * persisted - settings storage is not simulated) */
        for (int i = 0; i < 5; i++) {
            if (H.ble_host_present[i] != (i == 0)) {
                hf->api->ble_set_host_connected(i, H.ble_host_present[i]);
            }
        }
        if (H.leds) {
            H.leds_dirty = true;
        }
    }
}

static void run_half(struct half *hf, int64_t t) { hf->api->run_until(t - hf->t_offset); }

static int reload_half(struct half *hf);

static void check_reboot(struct half *hf) {
    if (hf->halted) {
        return;
    }
    int64_t r = hf->api->query(SIM_Q_REBOOT_REQUESTED);
    if (r < 0) {
        return;
    }
    if (r == 0x57) { /* RST_UF2: &bootloader */
        host_log("%s rebooted into the UF2 bootloader: its firmware has stopped (the sim keeps showing "
                 "the last image); restart the simulator to continue",
                 hf->name);
        hf->halted = true;
        return;
    }
    host_log("%s requested sys_reboot(%lld): reloading its DLL (RAM state lost, as on hardware)",
             hf->name, (long long)r);
    if (reload_half(hf) != 0) {
        exit(3);
    }
    hf->reboots++;
}

/* Cold-boot a half from (a fresh copy of) its DLL: the reboot path, and --watch
 * after a rebuild.  The new copy is loaded before the old one is released, so
 * a broken DLL leaves the running firmware alone. */
static int reload_half(struct half *hf) {
    char err[1400];
    struct half old = *hf;
    if (load_half_from(hf, err, sizeof(err)) != 0) {
        host_logf(1, "%s: %s", hf->name, err);
        return -1;
    }
    if (old.module) {
        FreeLibrary(old.module);
        DeleteFileA(old.shadow_path);
    }
    hf->halted = false;
    boot_half(hf);
    /* the matrix scan after boot sees switches that are still held */
    for (int pos = 0; pos < SIM_NUM_KEYS; pos++) {
        if (H.key_down[pos] && (sim_key_pos[pos].half ? HALF_PERIPHERAL : HALF_CENTRAL) == hf->role) {
            hf->api->position_event((uint32_t)pos, true, 0xFF);
        }
    }
    return 0;
}

/* --watch: poll the DLL files; once a changed file has been stable for a
 * moment (the linker is done writing it), cold-boot that half from it. */
void host_watch_poll(void) {
    static unsigned long long last_poll, exe_stamp;
    static bool exe_warned;
    if (!H.watch) {
        return;
    }
    unsigned long long now = GetTickCount64();
    if (now - last_poll < 250) {
        return;
    }
    last_poll = now;
    for (int i = 0; i < 2; i++) {
        struct half *hf = &H.half[i];
        unsigned long long size = 0, st = file_stamp(hf->dll_path, &size);
        if (st == 0 || st == hf->loaded_stamp) {
            hf->pending_stamp = 0;
            continue;
        }
        if (st != hf->pending_stamp) {
            hf->pending_stamp = st;
            hf->pending_since = now;
            continue;
        }
        if (now - hf->pending_since < 500) {
            continue;
        }
        host_log("%s: %s was rebuilt - reloading (cold boot of this half)", hf->name, hf->dll_path);
        if (reload_half(hf) == 0) {
            hf->reloads++;
        } else {
            hf->loaded_stamp = st; /* don't retry this build; the next one is picked up */
        }
        hf->pending_stamp = 0;
    }
    char exe[1000];
    snprintf(exe, sizeof(exe), "%s\\sim.exe", exe_dir);
    unsigned long long es = file_stamp(exe, NULL);
    if (!exe_stamp) {
        exe_stamp = es;
    } else if (es != exe_stamp && !exe_warned) {
        exe_warned = true;
        host_log("sim.exe itself was rebuilt (host code, keymap layout or display size changed): "
                 "restart the simulator to use it");
    }
}

/* ----------------------------------------------------- computer model */

static const char *usage_text(uint8_t u, bool shift, bool caps) {
    static char buf[8];
    static const char *num = "1234567890", *numsh = "!@#$%^&*()";
    if (u >= 0x04 && u <= 0x1d) {
        char c = 'a' + (u - 0x04);
        if (shift ^ caps) {
            c = c - 'a' + 'A';
        }
        buf[0] = c;
        buf[1] = 0;
        return buf;
    }
    if (u >= 0x1e && u <= 0x27) {
        buf[0] = (shift ? numsh : num)[u - 0x1e];
        buf[1] = 0;
        return buf;
    }
    static const struct {
        uint8_t u;
        const char *n, *s;
    } t[] = {
        {0x28, "\n", "\n"},   {0x29, "[Esc]", "[Esc]"}, {0x2b, "\t", "\t"},  {0x2c, " ", " "},
        {0x2d, "-", "_"},     {0x2e, "=", "+"},         {0x2f, "[", "{"},    {0x30, "]", "}"},
        {0x31, "\\", "|"},    {0x33, ";", ":"},         {0x34, "'", "\""},   {0x35, "`", "~"},
        {0x36, ",", "<"},     {0x37, ".", ">"},         {0x38, "/", "?"},    {0x4c, "[Del]", "[Del]"},
        {0x4f, "[Right]", "[Right]"}, {0x50, "[Left]", "[Left]"}, {0x51, "[Down]", "[Down]"},
        {0x52, "[Up]", "[Up]"}, {0x4a, "[Home]", "[Home]"}, {0x4d, "[End]", "[End]"},
        {0x4b, "[PgUp]", "[PgUp]"}, {0x4e, "[PgDn]", "[PgDn]"},
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        if (t[i].u == u) {
            return shift ? t[i].s : t[i].n;
        }
    }
    if (u >= 0x3a && u <= 0x45) {
        snprintf(buf, sizeof(buf), "[F%d]", u - 0x3a + 1);
        return buf;
    }
    return NULL;
}

static void typed_append(const char *s) {
    size_t n = strlen(s);
    if (H.typed_len + (int)n >= (int)sizeof(H.typed) - 1) {
        int drop = (int)sizeof(H.typed) / 2;
        memmove(H.typed, H.typed + drop, H.typed_len - drop);
        H.typed_len -= drop;
    }
    memcpy(H.typed + H.typed_len, s, n);
    H.typed_len += (int)n;
    H.typed[H.typed_len] = 0;
}

static void os_key_down(uint8_t u, uint8_t mods) {
    switch (u) {
    case 0x39:
        H.leds ^= 0x02;
        H.leds_dirty = true;
        break;
    case 0x53:
        H.leds ^= 0x01;
        H.leds_dirty = true;
        break;
    case 0x47:
        H.leds ^= 0x04;
        H.leds_dirty = true;
        break;
    case 0x2a: /* backspace */
        if (H.typed_len > 0) {
            H.typed[--H.typed_len] = 0;
        }
        return;
    default:
        break;
    }
    bool shift = mods & 0x22, chord = mods & 0xdd; /* ctrl/alt/gui */
    const char *s = usage_text(u, shift, H.leds & 0x02);
    if (!s) {
        return;
    }
    if (chord) {
        char b[48], m[48];
        snprintf(b, sizeof(b), "[%s+%s]", host_mods_str(mods & 0xdd, m, sizeof(m)), s);
        typed_append(b);
    } else {
        typed_append(s);
    }
}

static void os_report(const uint8_t *r, size_t len) {
    if (len < 2) {
        return;
    }
    uint8_t mods = r[0];
    uint8_t keys[256];
    int nk = 0;
    if (len > 16) { /* NKRO bitmap */
        for (size_t i = 2; i < len; i++) {
            for (int b = 0; b < 8; b++) {
                if (r[i] & (1 << b) && nk < 32) {
                    keys[nk++] = (uint8_t)((i - 2) * 8 + b);
                }
            }
        }
    } else {
        for (size_t i = 2; i < len; i++) {
            if (r[i]) {
                keys[nk++] = r[i];
            }
        }
    }
    for (int i = 0; i < nk; i++) {
        bool was = false;
        for (int j = 0; j < H.prev_nkeys; j++) {
            was |= H.prev_keys[j] == keys[i];
        }
        if (!was) {
            os_key_down(keys[i], mods);
        }
    }
    H.prev_nkeys = nk > 32 ? 32 : nk;
    memcpy(H.prev_keys, keys, H.prev_nkeys);
    H.mods = mods;
}

/* ------------------------------------------------------------- inputs */

void host_key(int pos, bool pressed) {
    if (pos < 0 || pos >= SIM_NUM_KEYS) {
        return;
    }
    if (H.key_down[pos] == pressed) {
        return;
    }
    H.key_down[pos] = pressed;
    /* The switch belongs to one half's matrix; that half's kscan reports it.
     * Left keys: central (local source).  Right keys: the peripheral, whose
     * real split/peripheral.c forwards them to the central over the link. */
    struct half *hf = &H.half[sim_key_pos[pos].half ? HALF_PERIPHERAL : HALF_CENTRAL];
    host_logf(4, "key %d %s (%s half)", pos, pressed ? "down" : "up",
              hf->role == HALF_CENTRAL ? "left" : "right");
    hf->api->position_event((uint32_t)pos, pressed, 0xFF);
}

void host_key_ref(int pos, bool pressed) {
    if (pos < 0 || pos >= SIM_NUM_KEYS) {
        return;
    }
    if (pressed) {
        if (H.key_refs[pos]++ == 0) {
            host_key(pos, true);
        }
    } else if (H.key_refs[pos] > 0) {
        if (--H.key_refs[pos] == 0) {
            host_key(pos, false);
        }
    }
}

void host_set_battery(int half, int pct) {
    pct = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    H.half[half].battery = pct;
    H.half[half].api->set_battery((uint8_t)pct);
}

void host_set_usb(int half, int state) {
    H.half[half].usb = state;
    H.half[half].api->set_usb(state);
}

void host_set_split(bool c) {
    if (c == H.split_connected) {
        return;
    }
    H.split_connected = c;
    if (!c) {
        H.qn = 0; /* in-flight packets are lost with the link */
    }
    H.half[0].api->set_split_connected(c);
    H.half[1].api->set_split_connected(c);
    host_logf(3, "split link %s", c ? "connected" : "disconnected");
}

void host_set_leds(uint8_t leds) {
    if (leds != H.leds) {
        H.leds = leds;
        H.leds_dirty = true;
    }
}

void host_ble_select(int idx) { H.half[0].api->ble_select_profile(idx); }
void host_ble_host_present(int idx, bool present) {
    if (idx < 0 || idx >= 5) {
        return;
    }
    H.ble_host_present[idx] = present;
    H.half[0].api->ble_set_host_connected(idx, present);
}
void host_ble_bond(int idx, bool bonded) { H.half[0].api->ble_set_bonded(idx, bonded); }
int host_select_output(int transport) { return H.half[0].api->select_transport(transport); }
int64_t host_query(int half, int what) { return H.half[half].api->query(what); }

/* ------------------------------------------------------------ auto typer */

static const int autotype_seq[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}; /* q..p across both halves */

void host_autotype_set(int wpm) {
    if (H.autotype_pending_release >= 0) {
        host_key_ref(H.autotype_pending_release, false);
        H.autotype_pending_release = -1;
    }
    H.autotype_wpm = wpm < 0 ? 0 : wpm;
    H.autotype_next = H.t + 1;
}

static void autotype_tick(int64_t t) {
    if (H.autotype_pending_release >= 0 && t >= H.autotype_release_at) {
        host_key_ref(H.autotype_pending_release, false);
        H.autotype_pending_release = -1;
    }
    if (H.autotype_wpm <= 0 || t < H.autotype_next) {
        return;
    }
    /* ZMK's WPM counts 5 key presses as a word */
    int interval = 60000 / (H.autotype_wpm * 5);
    if (interval < 40) {
        interval = 40;
    }
    int pos = autotype_seq[H.autotype_idx++ % (int)(sizeof(autotype_seq) / sizeof(int))];
    if (H.autotype_pending_release < 0) {
        host_key_ref(pos, true);
        H.autotype_pending_release = pos;
        H.autotype_release_at = t + (interval / 2 < 30 ? interval / 2 : 30);
    }
    H.autotype_next = t + interval;
}

/* ------------------------------------------------------------ lock-step */

static void deliver_packets(int64_t t) {
    for (int iter = 0; iter < 64; iter++) {
        bool any = false;
        bool ran[2] = {false, false};
        int i = 0;
        while (i < H.qn) {
            struct split_packet *p = &H.q[i];
            if (p->deliver_at > t) {
                i++;
                continue;
            }
            struct split_packet pk = *p;
            memmove(&H.q[i], &H.q[i + 1], (H.qn - i - 1) * sizeof(H.q[0]));
            H.qn--;
            H.half[pk.to].api->split_rx(pk.data, pk.len);
            ran[pk.to] = true;
            any = true;
        }
        if (!any) {
            break;
        }
        for (int h = 0; h < 2; h++) {
            if (ran[h]) {
                run_half(&H.half[h], t); /* handle it in the same ms */
            }
        }
    }
}

static void step_one(int64_t t) {
    H.t = t;
    script_run_due(t);
    autotype_tick(t);
    run_half(&H.half[0], t);
    run_half(&H.half[1], t);
    deliver_packets(t);
    if (H.leds_dirty) {
        H.leds_dirty = false;
        /* the computer sends its LED output report to the keyboard */
        H.half[0].api->hid_indicators_report(H.leds);
        run_half(&H.half[0], t);
        deliver_packets(t);
    }
    check_reboot(&H.half[0]);
    check_reboot(&H.half[1]);
}

void host_step_to(int64_t t) {
    while (H.t < t && !H.quit) {
        step_one(H.t + 1);
    }
}

/* ------------------------------------------------------------ images */

#define COMP_BG 48

void host_composite_size(int scale, int *w, int *h) {
    int pw, ph;
    panel_rotated_size(H.half[0].panel.w, H.half[0].panel.h, H.rotation, &pw, &ph);
    int m = 4 * scale, gap = 16 * scale;
    *w = 2 * pw * scale + gap + 2 * m;
    *h = ph * scale + 2 * m;
}

void host_composite(int scale, uint8_t *dst) {
    int w, h, pw, ph;
    host_composite_size(scale, &w, &h);
    panel_rotated_size(H.half[0].panel.w, H.half[0].panel.h, H.rotation, &pw, &ph);
    memset(dst, COMP_BG, (size_t)w * h);
    int m = 4 * scale, gap = 16 * scale;
    panel_blit(&H.half[0].panel, H.t * 1000, H.rotation, scale, dst, w, m, m);
    panel_blit(&H.half[1].panel, H.t * 1000, H.rotation, scale, dst, w, m + pw * scale + gap, m);
}

static void mkdirs_for(const char *path) {
    char tmp[1024];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = 0;
            _mkdir(tmp);
            *p = c;
        }
    }
}

int host_save_screenshot(const char *path) {
    int w, h;
    host_composite_size(H.scale, &w, &h);
    uint8_t *img = malloc((size_t)w * h);
    host_composite(H.scale, img);
    mkdirs_for(path);
    int r = png_write_grey(path, img, w, h);
    free(img);
    if (r == 0) {
        host_log("screenshot saved: %s", path);
    } else {
        host_log("ERROR: could not write %s", path);
    }
    return r;
}

/* ------------------------------------------------------------- headless */

struct capture {
    struct gif_writer *gif;
    const char *frames_dir;
    uint8_t *prev, *cur;
    int64_t prev_t;
    bool have_prev;
    int w, h;
    int frames;
};

static void capture_frame(struct capture *c, bool force) {
    bool busy = false;
    for (int i = 0; i < 2; i++) {
        busy |= H.half[i].dirty || H.half[i].panel.model->busy(&H.half[i].panel, H.t * 1000);
        H.half[i].dirty = false;
    }
    if (!busy && !force) {
        return;
    }
    host_composite(H.scale, c->cur);
    if (c->have_prev && memcmp(c->cur, c->prev, (size_t)c->w * c->h) == 0) {
        return;
    }
    if (c->frames_dir) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/frame_%07lldms.png", c->frames_dir, (long long)H.t);
        png_write_grey(path, c->cur, c->w, c->h);
    }
    if (c->gif && c->have_prev) {
        int cs = (int)((H.t + 5) / 10 - (c->prev_t + 5) / 10);
        if (cs > 0) {
            gif_frame(c->gif, c->prev, cs);
        }
        /* cs == 0: the previous image was visible < 10 ms; replace it */
    }
    uint8_t *t = c->prev;
    c->prev = c->cur;
    c->cur = t;
    c->prev_t = H.t;
    c->have_prev = true;
    c->frames++;
}

static int run_headless(const char *script, int64_t duration, const char *png, const char *gif,
                        const char *frames_dir) {
    struct capture cap = {0};
    host_composite_size(H.scale, &cap.w, &cap.h);
    if (gif || frames_dir) {
        cap.prev = malloc((size_t)cap.w * cap.h);
        cap.cur = malloc((size_t)cap.w * cap.h);
        if (frames_dir) {
            char probe[1024];
            snprintf(probe, sizeof(probe), "%s/x", frames_dir);
            mkdirs_for(probe);
            cap.frames_dir = frames_dir;
        }
        if (gif) {
            mkdirs_for(gif);
            cap.gif = gif_open(gif, cap.w, cap.h);
            if (!cap.gif) {
                fprintf(stderr, "sim: cannot write %s\n", gif);
                return 2;
            }
        }
        capture_frame(&cap, true);
    }
    int64_t done_at = -1;
    while (!H.quit) {
        if (duration >= 0 && H.t >= duration) {
            break;
        }
        if (duration < 0) {
            if (!script && H.t >= 10000) {
                break;
            }
            if (script && script_done()) {
                if (done_at < 0) {
                    done_at = H.t;
                }
                if (H.t >= done_at + 1000) {
                    break;
                }
            }
        }
        step_one(H.t + 1);
        if (cap.prev) {
            capture_frame(&cap, false);
        }
    }
    if (cap.gif) {
        int cs = (int)((H.t + 5) / 10 - (cap.prev_t + 5) / 10);
        gif_frame(cap.gif, cap.prev, cs < 2 ? 2 : cs);
        gif_close(cap.gif);
        host_log("GIF written: %s (%d frames)", gif, cap.frames);
    }
    if (png) {
        host_save_screenshot(png);
    }
    char mods[64];
    host_log("end of run at t=%.3f s: layer %s, WPM %lld, mods %s, LEDs 0x%llx, writes C=%lld "
             "P=%lld, LVGL heap peak C=%lld P=%lld",
             H.t / 1000.0, host_layer_name((int)host_query(0, SIM_Q_HIGHEST_LAYER)),
             (long long)host_query(0, SIM_Q_WPM),
             host_mods_str((uint8_t)host_query(0, SIM_Q_EXPLICIT_MODS), mods, sizeof(mods)),
             (long long)host_query(0, SIM_Q_HID_INDICATORS),
             (long long)host_query(0, SIM_Q_DISPLAY_WRITES),
             (long long)host_query(1, SIM_Q_DISPLAY_WRITES),
             (long long)host_query(0, SIM_Q_LVGL_HEAP_PEAK),
             (long long)host_query(1, SIM_Q_LVGL_HEAP_PEAK));
    if (H.typed_len) {
        host_log("computer received text: \"%s\"", H.typed);
    }
    if (H.expect_passed || H.expect_failed) {
        host_log("expectations: %d passed, %d failed", H.expect_passed, H.expect_failed);
    }
    return H.expect_failed ? 1 : 0;
}

/* ---------------------------------------------------------------- main */

static void usage(void) {
    printf(
        "usage: sim.exe [options]\n"
        "  --headless            no window; run the script / duration and exit\n"
        "  --script FILE         timed input script (.txt or .json, see sim/README.md)\n"
        "  --duration MS         virtual time to run headless (default: script end + 1 s, or 10 s)\n"
        "  --png FILE            headless: write the final screens as PNG\n"
        "  --gif FILE            headless: record every screen change into an animated GIF\n"
        "  --frames DIR          headless: write every distinct frame as DIR/frame_<ms>.png\n"
        "  --scale N             image scale for PNG/GIF/screenshots (default 2)\n"
        "  --panel ideal|memlcd  display model (default ideal; memlcd = Sharp memory LCD timing)\n"
        "  --memlcd-response-ms N  memlcd: pixel response time (default 0, uncalibrated)\n"
        "  --rotate DEG          clockwise view rotation of the 160x68 panel (default 270)\n"
        "  --battery-central N, --battery-peripheral N   initial battery %% (default 80 / 60)\n"
        "  --usb-central S, --usb-peripheral S           initial USB state none|powered|hid\n"
        "  --split-off           start with the split link down\n"
        "  --split-latency MS    delay split packets (default 0)\n"
        "  --log-level N         console log level 0..4 (default 2 = warnings)\n"
        "  --log FILE            also write all logs (debug level) to FILE\n"
        "  --shot-dir DIR        where window screenshots go (default sim/build/screenshots)\n"
        "  --central DLL, --peripheral DLL   MCU DLLs (default: next to sim.exe)\n"
        "  --watch               window: when central.dll / peripheral.dll are rebuilt (make sim-build),\n"
        "                        cold-boot that half from the new DLL without restarting\n"
        "  --quiet               no console logging\n");
}

static int parse_usb(const char *s) {
    if (!strcmp(s, "hid")) {
        return SIM_USB_HID;
    }
    if (!strcmp(s, "powered")) {
        return SIM_USB_POWERED;
    }
    return SIM_USB_NONE;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 4096);
    GetModuleFileNameA(NULL, exe_dir, sizeof(exe_dir));
    char *slash = strrchr(exe_dir, '\\');
    if (slash) {
        *slash = 0;
    }

    const char *script = NULL, *png = NULL, *gif = NULL, *frames = NULL, *logpath = NULL;
    const char *central_dll = NULL, *periph_dll = NULL;
    bool headless = false;
    int64_t duration = -1;
    H.scale = 2;
    H.rotation = 270;
    H.panel_model = &panel_model_ideal;
    H.log_level = 2;
    H.split_connected = true;
    H.autotype_pending_release = -1;
    H.ble_host_present[0] = true;
    snprintf(H.shot_dir, sizeof(H.shot_dir), "sim/build/screenshots");
    H.half[0].battery = 80;
    H.half[1].battery = 60;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
#define ARGV() (v ? (i++, v) : (fprintf(stderr, "sim: %s needs a value\n", a), exit(2), ""))
        if (!strcmp(a, "--help") || !strcmp(a, "-h")) {
            usage();
            return 0;
        } else if (!strcmp(a, "--headless")) {
            headless = true;
        } else if (!strcmp(a, "--script")) {
            script = ARGV();
        } else if (!strcmp(a, "--duration")) {
            duration = atoll(ARGV());
        } else if (!strcmp(a, "--png")) {
            png = ARGV();
        } else if (!strcmp(a, "--gif")) {
            gif = ARGV();
        } else if (!strcmp(a, "--frames")) {
            frames = ARGV();
        } else if (!strcmp(a, "--scale")) {
            H.scale = atoi(ARGV());
            if (H.scale < 1 || H.scale > 8) {
                H.scale = 2;
            }
        } else if (!strcmp(a, "--panel")) {
            const char *n = ARGV();
            if (*n == 0) {
                continue; /* PANEL_EMU unset */
            }
            H.panel_model = panel_model_by_name(n);
            if (!H.panel_model) {
                fprintf(stderr, "sim: unknown panel model '%s' (ideal|memlcd)\n", n);
                return 2;
            }
        } else if (!strcmp(a, "--memlcd-response-ms")) {
            memlcd_response_ms = atoi(ARGV());
        } else if (!strcmp(a, "--epaper")) {
            fprintf(stderr, "sim: --epaper / EPAPER_EMU was dropped: the overlay's panel is a Sharp "
                            "memory LCD; use --panel memlcd (PANEL_EMU=memlcd)\n");
            return 2;
        } else if (!strcmp(a, "--rotate")) {
            H.rotation = ((atoi(ARGV()) % 360) + 360) % 360;
            if (H.rotation % 90) {
                H.rotation = 270;
            }
        } else if (!strcmp(a, "--battery-central")) {
            H.half[0].battery = atoi(ARGV());
        } else if (!strcmp(a, "--battery-peripheral")) {
            H.half[1].battery = atoi(ARGV());
        } else if (!strcmp(a, "--usb-central")) {
            H.half[0].usb = parse_usb(ARGV());
        } else if (!strcmp(a, "--usb-peripheral")) {
            H.half[1].usb = parse_usb(ARGV());
        } else if (!strcmp(a, "--split-off")) {
            H.split_connected = false;
        } else if (!strcmp(a, "--split-latency")) {
            H.split_latency_ms = atoi(ARGV());
        } else if (!strcmp(a, "--log-level")) {
            H.log_level = atoi(ARGV());
        } else if (!strcmp(a, "--log")) {
            logpath = ARGV();
        } else if (!strcmp(a, "--shot-dir")) {
            snprintf(H.shot_dir, sizeof(H.shot_dir), "%s", ARGV());
        } else if (!strcmp(a, "--central")) {
            central_dll = ARGV();
        } else if (!strcmp(a, "--peripheral")) {
            periph_dll = ARGV();
        } else if (!strcmp(a, "--gen")) {
            ARGV(); /* accepted for compatibility; keymap info is compiled in */
        } else if (!strcmp(a, "--quiet")) {
            H.quiet = true;
        } else if (!strcmp(a, "--watch")) {
            H.watch = true;
        } else {
            fprintf(stderr, "sim: unknown option %s (try --help)\n", a);
            return 2;
        }
    }
    if (headless && H.watch) {
        fprintf(stderr, "sim: --watch is for the window (a headless run ends by itself)\n");
        return 2;
    }
    if (!headless && (png || gif || frames)) {
        fprintf(stderr, "sim: --png/--gif/--frames need --headless (use F12 for window screenshots)\n");
        return 2;
    }
    if (logpath) {
        mkdirs_for(logpath);
        H.log_file = fopen(logpath, "w");
    }

    H.half[0].role = HALF_CENTRAL;
    H.half[0].name = "central (left)";
    H.half[1].role = HALF_PERIPHERAL;
    H.half[1].name = "peripheral (right)";
    snprintf(H.half[0].dll_path, sizeof(H.half[0].dll_path), "%s",
             central_dll ? central_dll : "");
    snprintf(H.half[1].dll_path, sizeof(H.half[1].dll_path), "%s", periph_dll ? periph_dll : "");
    if (!central_dll) {
        snprintf(H.half[0].dll_path, sizeof(H.half[0].dll_path), "%s\\central.dll", exe_dir);
    }
    if (!periph_dll) {
        snprintf(H.half[1].dll_path, sizeof(H.half[1].dll_path), "%s\\peripheral.dll", exe_dir);
    }

    if (script && script_load(script) != 0) {
        return 2;
    }

    base_params.display_spi_timing = H.panel_model == &panel_model_memlcd;
    int dll_level = H.quiet ? 0 : H.log_level;
    if (H.log_file && file_log_level > dll_level) {
        dll_level = file_log_level;
    }
    base_params.log_level = dll_level;

    host_log("ZMK status screen simulator: panel model %s, view rotation %d, display %s %dx%d "
             "(spi %d Hz, extcomin %s)",
             H.panel_model->name, H.rotation, SIM_DISPLAY_COMPAT, SIM_DISPLAY_WIDTH,
             SIM_DISPLAY_HEIGHT, SIM_DISPLAY_SPI_HZ, SIM_DISPLAY_EXTCOMIN ? "yes" : "no");
    clean_shadow_dir();
    for (int i = 0; i < 2; i++) {
        if (load_half(&H.half[i]) != 0) {
            return 3;
        }
    }
    boot_half(&H.half[0]);
    boot_half(&H.half[1]);
    deliver_packets(0);

    int rc = headless ? run_headless(script, duration, png, gif, frames) : ui_run();
    if (H.log_file) {
        fclose(H.log_file);
    }
    /* skip DLL teardown: firmware has no notion of exit */
    fflush(stdout);
    ExitProcess(rc);
    return rc;
}
