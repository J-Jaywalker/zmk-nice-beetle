/*
 * Scripted input (text or JSON) and `expect` checks.
 *
 * The whole script is parsed and validated at load time into a queue of timed
 * entries (host ms, command text, "file:line"), stable-sorted by time.
 * Commands that create future events (tap release, type) insert into the same
 * queue when they run.  See script_load() for the syntax.
 */
#include "host.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------ queue */

struct entry {
    int64_t t;
    long seq;
    char *text;
    char where[160];
    bool checked; /* validated at load: don't repeat warnings */
};

static struct entry *Q;
static int qh, qn, qcap; /* live entries are Q[qh..qn) */
static long seq_ctr;
static int64_t end_time;

static int q_insert(int64_t t, const char *text, const char *where, bool checked)
{
    if (qn == qcap) {
        if (qh > 0) {
            memmove(Q, Q + qh, sizeof(*Q) * (size_t)(qn - qh));
            qn -= qh;
            qh = 0;
        } else {
            int cap = qcap ? qcap * 2 : 256;
            struct entry *n = realloc(Q, sizeof(*Q) * (size_t)cap);
            if (!n)
                return -1;
            Q = n;
            qcap = cap;
        }
    }
    /* upper bound on t within [qh, qn): equal times keep insertion order */
    int lo = qh, hi = qn;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (Q[mid].t <= t)
            lo = mid + 1;
        else
            hi = mid;
    }
    memmove(Q + lo + 1, Q + lo, sizeof(*Q) * (size_t)(qn - lo));
    struct entry *e = &Q[lo];
    e->t = t;
    e->seq = seq_ctr++;
    e->text = strdup(text);
    snprintf(e->where, sizeof e->where, "%s", where ? where : "?");
    e->checked = checked;
    qn++;
    if (t > end_time)
        end_time = t;
    return e->text ? 0 : -1;
}

/* ------------------------------------------------------------ helpers */

enum { M_CHECK, M_EXEC, M_EXEC_QUIET };

static void report(int mode, const char *where, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
static void report(int mode, const char *where, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (mode == M_CHECK)
        fprintf(stderr, "%s: %s\n", where, msg);
    else
        host_log("script: %s: %s", where, msg);
}

static const char *skip_ws(const char *p)
{
    while (*p && isspace((unsigned char)*p))
        p++;
    return p;
}

/* next whitespace-separated token; false at end of string */
static bool next_tok(const char **pp, char *out, size_t n)
{
    const char *p = skip_ws(*pp);
    if (!*p) {
        *pp = p;
        return false;
    }
    size_t k = 0;
    while (*p && !isspace((unsigned char)*p)) {
        if (k + 1 < n)
            out[k++] = *p;
        p++;
    }
    out[k] = 0;
    *pp = p;
    return true;
}

/* rest of the line, trimmed; a fully "quoted" value is unquoted (\" \\ \n) */
static void rest_str(const char *p, char *out, size_t n)
{
    p = skip_ws(p);
    size_t len = strlen(p);
    while (len && isspace((unsigned char)p[len - 1]))
        len--;
    size_t k = 0;
    if (len >= 2 && p[0] == '"' && p[len - 1] == '"') {
        for (size_t i = 1; i + 1 < len && k + 1 < n; i++) {
            char c = p[i];
            if (c == '\\' && i + 2 < len) {
                c = p[++i];
                if (c == 'n')
                    c = '\n';
                else if (c == 't')
                    c = '\t';
            }
            out[k++] = c;
        }
    } else {
        for (size_t i = 0; i < len && k + 1 < n; i++)
            out[k++] = p[i];
    }
    out[k] = 0;
}

static bool parse_int(const char *s, long long *v)
{
    if (!s || !*s)
        return false;
    char *end;
    *v = strtoll(s, &end, 0);
    return *end == 0;
}

/* "<n>", "<n>ms" or "<x>s" (x may be fractional) -> ms, >= 0 */
static bool parse_ms(const char *s, int64_t *ms)
{
    if (!s || !isdigit((unsigned char)*s))
        return false;
    char *end;
    double v = strtod(s, &end);
    if (!strcmp(end, "") || !strcmp(end, "ms"))
        *ms = (int64_t)(v + 0.5);
    else if (!strcmp(end, "s"))
        *ms = (int64_t)(v * 1000.0 + 0.5);
    else
        return false;
    return true;
}

static bool parse_onoff(const char *s, bool *v)
{
    static const char *on[] = {"on", "true", "yes", "1", "connected", "present"};
    static const char *off[] = {"off", "false", "no", "0", "disconnected", "absent"};
    for (size_t i = 0; i < sizeof on / sizeof *on; i++) {
        if (!strcasecmp(s, on[i])) {
            *v = true;
            return true;
        }
        if (!strcasecmp(s, off[i])) {
            *v = false;
            return true;
        }
    }
    return false;
}

/* 0 central, 1 peripheral, 2 both */
static int parse_half(const char *s)
{
    if (!strcasecmp(s, "central") || !strcasecmp(s, "c") || !strcasecmp(s, "left") ||
        !strcasecmp(s, "l") || !strcmp(s, "0"))
        return 0;
    if (!strcasecmp(s, "peripheral") || !strcasecmp(s, "p") || !strcasecmp(s, "right") ||
        !strcasecmp(s, "r") || !strcmp(s, "1"))
        return 1;
    if (!strcasecmp(s, "both"))
        return 2;
    return -1;
}

static int parse_usb(const char *s)
{
    if (!strcasecmp(s, "none") || !strcasecmp(s, "off") || !strcmp(s, "0"))
        return SIM_USB_NONE;
    if (!strcasecmp(s, "powered") || !strcasecmp(s, "power") || !strcmp(s, "1"))
        return SIM_USB_POWERED;
    if (!strcasecmp(s, "hid") || !strcmp(s, "2"))
        return SIM_USB_HID;
    return -1;
}

/* LED bits: "none", a number, or names joined with + , or | (num caps scroll) */
static bool parse_leds(const char *s, long long *v)
{
    if (parse_int(s, v))
        return *v >= 0 && *v <= 255;
    *v = 0;
    if (!strcasecmp(s, "none"))
        return true;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", s);
    for (char *t = strtok(buf, "+,|"); t; t = strtok(NULL, "+,|")) {
        if (!strcasecmp(t, "num") || !strcasecmp(t, "numlock"))
            *v |= 1;
        else if (!strcasecmp(t, "caps") || !strcasecmp(t, "capslock"))
            *v |= 2;
        else if (!strcasecmp(t, "scroll") || !strcasecmp(t, "scrolllock"))
            *v |= 4;
        else
            return false;
    }
    return true;
}

/* modifier bits: "none", a number, or names joined with + , or | */
static bool parse_mods(const char *s, long long *v)
{
    static const char *names[8][3] = {
        {"lctrl", "lctl", "lc"}, {"lshift", "lsft", "ls"}, {"lalt", "la", "lopt"},
        {"lgui", "lg", "lcmd"},  {"rctrl", "rctl", "rc"},  {"rshift", "rsft", "rs"},
        {"ralt", "ra", "ropt"},  {"rgui", "rg", "rcmd"},
    };
    if (parse_int(s, v))
        return *v >= 0 && *v <= 255;
    *v = 0;
    if (!strcasecmp(s, "none"))
        return true;
    char buf[1024];
    snprintf(buf, sizeof buf, "%s", s);
    for (char *t = strtok(buf, "+,|"); t; t = strtok(NULL, "+,|")) {
        int bit = -1;
        for (int i = 0; i < 8 && bit < 0; i++)
            for (int k = 0; k < 3; k++)
                if (!strcasecmp(t, names[i][k]))
                    bit = i;
        if (bit < 0)
            return false;
        *v |= 1 << bit;
    }
    return true;
}

static bool parse_pos(const char *s, int *pos)
{
    long long v;
    if (!parse_int(s, &v) || v < 0)
        return false;
    int n = host_num_keys();
    if (n > 0 && v >= n)
        return false;
    if (v > 255)
        return false;
    *pos = (int)v;
    return true;
}

/* QWERTY character -> matrix position (Corne 42-key, see docs) */
static int char_pos(char c)
{
    static const char *rows[] = {"qwertyuiop", "asdfghjkl;'", "zxcvbnm,./"};
    static const int base[] = {1, 13, 25};
    for (int r = 0; r < 3; r++) {
        const char *p = strchr(rows[r], c);
        if (c && p)
            return base[r] + (int)(p - rows[r]);
    }
    if (c == ' ')
        return 39;
    return -1;
}

/* ------------------------------------------------------------ expect */

enum { K_NUM, K_LAYER, K_USB, K_TRANSPORT, K_BOOL, K_LEDS, K_MODS, K_ACTIVITY, K_TYPED };

struct qdef {
    const char *name;
    int half;
    int what;
    int kind;
};

static const struct qdef qdefs[] = {
    {"layer", 0, SIM_Q_HIGHEST_LAYER, K_LAYER},
    {"layer_state", 0, SIM_Q_LAYER_STATE, K_NUM},
    {"wpm", 0, SIM_Q_WPM, K_NUM},
    {"wpm_zmk", 0, SIM_Q_WPM_ZMK, K_NUM},
    {"mods", 0, SIM_Q_EXPLICIT_MODS, K_MODS},
    {"battery", 0, SIM_Q_BATTERY, K_NUM},
    {"battery.central", 0, SIM_Q_BATTERY, K_NUM},
    {"battery.peripheral", 1, SIM_Q_BATTERY, K_NUM},
    {"usb", 0, SIM_Q_USB_STATE, K_USB},
    {"usb.central", 0, SIM_Q_USB_STATE, K_USB},
    {"usb.peripheral", 1, SIM_Q_USB_STATE, K_USB},
    {"profile", 0, SIM_Q_ACTIVE_PROFILE, K_NUM},
    {"connected", 0, SIM_Q_PROFILE_CONNECTED, K_BOOL},
    {"bonded", 0, SIM_Q_PROFILE_BONDED, K_BOOL},
    {"transport", 0, SIM_Q_ENDPOINT_TRANSPORT, K_TRANSPORT},
    {"split", 0, SIM_Q_SPLIT_CONNECTED, K_BOOL},
    {"split.central", 0, SIM_Q_SPLIT_CONNECTED, K_BOOL},
    {"split.peripheral", 1, SIM_Q_SPLIT_CONNECTED, K_BOOL},
    {"leds", 0, SIM_Q_HID_INDICATORS, K_LEDS},
    {"writes", 0, SIM_Q_DISPLAY_WRITES, K_NUM},
    {"writes.central", 0, SIM_Q_DISPLAY_WRITES, K_NUM},
    {"writes.peripheral", 1, SIM_Q_DISPLAY_WRITES, K_NUM},
    {"heap", 0, SIM_Q_LVGL_HEAP_USED, K_NUM},
    {"heap.central", 0, SIM_Q_LVGL_HEAP_USED, K_NUM},
    {"heap.peripheral", 1, SIM_Q_LVGL_HEAP_USED, K_NUM},
    {"heap_peak", 0, SIM_Q_LVGL_HEAP_PEAK, K_NUM},
    {"heap_peak.central", 0, SIM_Q_LVGL_HEAP_PEAK, K_NUM},
    {"heap_peak.peripheral", 1, SIM_Q_LVGL_HEAP_PEAK, K_NUM},
    {"activity", 0, SIM_Q_ACTIVITY_STATE, K_ACTIVITY},
    {"typed", 0, -1, K_TYPED},
};

/* value for a numeric query kind; *ok false on syntax error */
static long long parse_value(int kind, const char *s, bool *ok)
{
    long long v = 0;
    *ok = true;
    switch (kind) {
    case K_LAYER: {
        if (parse_int(s, &v))
            return v;
        int n = host_num_layers();
        for (int i = 0; i < n; i++) {
            const char *name = host_layer_name(i);
            if (name && !strcasecmp(name, s))
                return i;
        }
        *ok = false;
        return 0;
    }
    case K_USB:
        v = parse_usb(s);
        *ok = v >= 0;
        return v;
    case K_TRANSPORT:
        if (!strcasecmp(s, "usb") || !strcmp(s, "0"))
            return 0;
        if (!strcasecmp(s, "ble") || !strcmp(s, "1"))
            return 1;
        if (!strcasecmp(s, "none") || !strcmp(s, "-1"))
            return -1;
        *ok = false;
        return 0;
    case K_BOOL: {
        bool b;
        if (parse_onoff(s, &b))
            return b;
        *ok = parse_int(s, &v);
        return v;
    }
    case K_LEDS:
        *ok = parse_leds(s, &v);
        return v;
    case K_MODS:
        *ok = parse_mods(s, &v);
        return v;
    case K_ACTIVITY:
        if (!strcasecmp(s, "active"))
            return 0;
        if (!strcasecmp(s, "idle"))
            return 1;
        if (!strcasecmp(s, "sleep"))
            return 2;
        *ok = parse_int(s, &v);
        return v;
    default:
        *ok = parse_int(s, &v);
        return v;
    }
}

static const char *ops[] = {"==", "!=", "<", "<=", ">", ">=", "contains", "="};

static void describe(int kind, long long v, char *buf, size_t n)
{
    switch (kind) {
    case K_LAYER: {
        const char *name = host_layer_name((int)v);
        snprintf(buf, n, "%lld (%s)", v, name ? name : "?");
        break;
    }
    case K_USB:
        snprintf(buf, n, "%s", host_usb_name((int)v));
        break;
    case K_TRANSPORT:
        snprintf(buf, n, "%s", v == 0 ? "usb" : v == 1 ? "ble" : v == -1 ? "none" : "?");
        break;
    case K_ACTIVITY:
        snprintf(buf, n, "%s", v == 0 ? "active" : v == 1 ? "idle" : v == 2 ? "sleep" : "?");
        break;
    case K_LEDS:
        snprintf(buf, n, "%lld (%s%s%s)", v, (v & 1) ? "num " : "", (v & 2) ? "caps " : "",
                 (v & 4) ? "scroll " : "");
        break;
    case K_MODS: {
        char m[64];
        const char *s = host_mods_str((uint8_t)v, m, sizeof m);
        snprintf(buf, n, "0x%02llx (%s)", v, s ? s : "");
        break;
    }
    default:
        snprintf(buf, n, "%lld", v);
    }
}

static int cmd_expect(const char *args, const char *where, int mode)
{
    char qname[64], op[16], val[1024];
    const char *p = args;
    if (!next_tok(&p, qname, sizeof qname) || !next_tok(&p, op, sizeof op)) {
        report(mode, where, "error: usage: expect <query> <op> <value>");
        return -1;
    }
    rest_str(p, val, sizeof val);
    const struct qdef *q = NULL;
    for (size_t i = 0; i < sizeof qdefs / sizeof *qdefs; i++)
        if (!strcasecmp(qdefs[i].name, qname))
            q = &qdefs[i];
    if (!q) {
        report(mode, where, "error: unknown expect query '%s'", qname);
        return -1;
    }
    int oi = -1;
    for (int i = 0; i < (int)(sizeof ops / sizeof *ops); i++)
        if (!strcmp(op, ops[i]))
            oi = i;
    if (oi == 7)
        oi = 0; /* "=" */
    if (oi < 0 || (q->kind == K_TYPED && oi != 0 && oi != 1 && oi != 6) ||
        (q->kind != K_TYPED && oi == 6)) {
        report(mode, where, "error: invalid operator '%s' for '%s'", op, q->name);
        return -1;
    }
    if (q->kind != K_TYPED && !val[0]) {
        report(mode, where, "error: expect %s %s: missing value", q->name, op);
        return -1;
    }

    long long want = 0;
    if (q->kind != K_TYPED) {
        bool ok;
        want = parse_value(q->kind, val, &ok);
        /* layer names cannot be checked before the keymap is known */
        if (!ok && !(q->kind == K_LAYER && host_num_layers() <= 0)) {
            report(mode, where, "error: invalid value '%s' for '%s'", val, q->name);
            return -1;
        }
        if (!ok && mode != M_CHECK)
            return -1;
    }
    if (mode == M_CHECK)
        return 0;

    bool pass;
    char actual[200];
    if (q->kind == K_TYPED) {
        int n = H.typed_len;
        if (n < 0)
            n = 0;
        if (n > (int)sizeof H.typed - 1)
            n = (int)sizeof H.typed - 1;
        char typed[sizeof H.typed];
        memcpy(typed, H.typed, (size_t)n);
        typed[n] = 0;
        if (oi == 6)
            pass = strstr(typed, val) != NULL;
        else
            pass = (strcmp(typed, val) == 0) == (oi == 0);
        const char *tail = n > 120 ? typed + n - 120 : typed;
        snprintf(actual, sizeof actual, "\"%s%.150s\"", n > 120 ? "..." : "", tail);
    } else {
        long long have = host_query(q->half, q->what);
        switch (oi) {
        case 0: pass = have == want; break;
        case 1: pass = have != want; break;
        case 2: pass = have < want; break;
        case 3: pass = have <= want; break;
        case 4: pass = have > want; break;
        default: pass = have >= want; break;
        }
        describe(q->kind, have, actual, sizeof actual);
    }
    if (pass)
        H.expect_passed++;
    else
        H.expect_failed++;
    host_log("script: %s %s: expect %s %s %s (actual %s)", pass ? "PASS" : "FAIL", where, q->name,
             op, val, actual);
    return 0;
}

/* ------------------------------------------------------------ commands */

#define TAP_GAP_MS 30
#define TYPE_HOLD_MS 40
#define TYPE_GAP_MS 40

/*
 * Validate (M_CHECK) or execute one command at time t.  *dur receives how far
 * the command moves the load-time cursor (tap/type).  Returns 0 or -1.
 */
static int run_cmd(const char *cmd, int64_t t, const char *where, int mode, int64_t *dur)
{
    char name[32], a1[64], a2[64], a3[64], extra[64];
    const char *p = cmd;
    bool exec = mode != M_CHECK;
    *dur = 0;
    if (!next_tok(&p, name, sizeof name))
        return 0;
    for (char *c = name; *c; c++)
        *c = (char)tolower((unsigned char)*c);

    if (!strcmp(name, "press") || !strcmp(name, "release")) {
        int pos;
        if (!next_tok(&p, a1, sizeof a1) || !parse_pos(a1, &pos) || next_tok(&p, extra, sizeof extra)) {
            report(mode, where, "error: usage: %s <pos 0..%d>", name, host_num_keys() - 1);
            return -1;
        }
        if (exec)
            host_key_ref(pos, name[0] == 'p');
        return 0;
    }
    if (!strcmp(name, "pckey")) {
        /* a key of the PC keyboard, through the window's mapping (ui.c), e.g. pckey lalt down */
        bool down;
        if (!next_tok(&p, a1, sizeof a1) || !next_tok(&p, a2, sizeof a2) ||
            next_tok(&p, extra, sizeof extra) ||
            !((down = !strcmp(a2, "down")) || !strcmp(a2, "up"))) {
            report(mode, where, "error: usage: pckey <lalt|lctrl|lshift|lgui|SDL_key_name> down|up");
            return -1;
        }
        if (exec && ui_pc_key(a1, down) != 0)
            report(mode, where, "warning: pckey: '%s' is not mapped to a key", a1);
        return 0;
    }
    if (!strcmp(name, "tap")) {
        int pos;
        int64_t hold = TYPE_HOLD_MS;
        if (!next_tok(&p, a1, sizeof a1) || !parse_pos(a1, &pos) ||
            (next_tok(&p, a2, sizeof a2) && !parse_ms(a2, &hold)) || next_tok(&p, extra, sizeof extra)) {
            report(mode, where, "error: usage: tap <pos 0..%d> [hold_ms]", host_num_keys() - 1);
            return -1;
        }
        *dur = hold + TAP_GAP_MS;
        if (exec) {
            char rel[32];
            host_key_ref(pos, true);
            snprintf(rel, sizeof rel, "release %d", pos);
            q_insert(t + hold, rel, where, true);
        }
        return 0;
    }
    if (!strcmp(name, "type")) {
        char text[1024];
        rest_str(p, text, sizeof text);
        int64_t at = t;
        for (const char *c = text; *c; c++) {
            int pos = char_pos(*c);
            if (pos < 0) {
                if (mode != M_EXEC_QUIET)
                    report(mode, where, "warning: type: no key for character '%c' (0x%02x), skipped",
                           isprint((unsigned char)*c) ? *c : '?', (unsigned char)*c);
                continue;
            }
            if (exec) {
                char ev[32];
                snprintf(ev, sizeof ev, "press %d", pos);
                if (at == t)
                    host_key_ref(pos, true);
                else
                    q_insert(at, ev, where, true);
                snprintf(ev, sizeof ev, "release %d", pos);
                q_insert(at + TYPE_HOLD_MS, ev, where, true);
            }
            at += TYPE_HOLD_MS + TYPE_GAP_MS;
        }
        *dur = at - t;
        return 0;
    }
    if (!strcmp(name, "battery")) {
        long long pct;
        int half = -1;
        if (!next_tok(&p, a1, sizeof a1) || (half = parse_half(a1)) < 0 ||
            !next_tok(&p, a2, sizeof a2) || !parse_int(a2, &pct) || pct < 0 || pct > 100 ||
            next_tok(&p, extra, sizeof extra)) {
            report(mode, where, "error: usage: battery <central|peripheral|both> <0..100>");
            return -1;
        }
        if (exec)
            for (int h = 0; h < 2; h++)
                if (half == 2 || half == h)
                    host_set_battery(h == 0 ? HALF_CENTRAL : HALF_PERIPHERAL, (int)pct);
        return 0;
    }
    if (!strcmp(name, "usb")) {
        int half = -1, st = -1;
        if (!next_tok(&p, a1, sizeof a1) || (half = parse_half(a1)) < 0 ||
            !next_tok(&p, a2, sizeof a2) || (st = parse_usb(a2)) < 0 || next_tok(&p, extra, sizeof extra)) {
            report(mode, where, "error: usage: usb <central|peripheral|both> none|powered|hid");
            return -1;
        }
        if (exec)
            for (int h = 0; h < 2; h++)
                if (half == 2 || half == h)
                    host_set_usb(h == 0 ? HALF_CENTRAL : HALF_PERIPHERAL, st);
        return 0;
    }
    if (!strcmp(name, "ble")) {
        long long idx;
        bool on = false;
        bool ok = next_tok(&p, a1, sizeof a1) && next_tok(&p, a2, sizeof a2) && parse_int(a2, &idx) &&
                  idx >= 0 && idx < 5;
        if (ok && !strcasecmp(a1, "select"))
            ok = !next_tok(&p, extra, sizeof extra);
        else if (ok && (!strcasecmp(a1, "host") || !strcasecmp(a1, "bond")))
            ok = next_tok(&p, a3, sizeof a3) && parse_onoff(a3, &on) && !next_tok(&p, extra, sizeof extra);
        else
            ok = false;
        if (!ok) {
            report(mode, where, "error: usage: ble select <0..4> | ble host <0..4> on|off | ble bond <0..4> on|off");
            return -1;
        }
        if (exec) {
            if (!strcasecmp(a1, "select"))
                host_ble_select((int)idx);
            else if (!strcasecmp(a1, "host"))
                host_ble_host_present((int)idx, on);
            else
                host_ble_bond((int)idx, on);
        }
        return 0;
    }
    if (!strcmp(name, "output")) {
        int tr = -1;
        if (next_tok(&p, a1, sizeof a1)) {
            if (!strcasecmp(a1, "usb") || !strcmp(a1, "0"))
                tr = 0;
            else if (!strcasecmp(a1, "ble") || !strcmp(a1, "1"))
                tr = 1;
        }
        if (tr < 0 || next_tok(&p, extra, sizeof extra)) {
            report(mode, where, "error: usage: output usb|ble");
            return -1;
        }
        if (exec) {
            int rc = host_select_output(tr);
            if (rc < 0)
                report(mode, where, "warning: output %s: firmware returned %d", a1, rc);
        }
        return 0;
    }
    if (!strcmp(name, "split")) {
        bool on;
        if (!next_tok(&p, a1, sizeof a1) || !parse_onoff(a1, &on) || next_tok(&p, extra, sizeof extra)) {
            report(mode, where, "error: usage: split on|off");
            return -1;
        }
        if (exec)
            host_set_split(on);
        return 0;
    }
    if (!strcmp(name, "leds")) {
        long long bits = 0, v;
        int n = 0;
        while (next_tok(&p, a1, sizeof a1)) {
            if (!parse_leds(a1, &v)) {
                report(mode, where, "error: leds: unknown '%s' (use none, num, caps, scroll or a number)", a1);
                return -1;
            }
            bits |= v;
            n++;
        }
        if (!n) {
            report(mode, where, "error: usage: leds none | leds [caps] [num] [scroll]");
            return -1;
        }
        if (exec)
            host_set_leds((uint8_t)bits);
        return 0;
    }
    if (!strcmp(name, "autotype")) {
        long long wpm;
        bool off;
        if (!next_tok(&p, a1, sizeof a1)) {
            report(mode, where, "error: usage: autotype <wpm>|off");
            return -1;
        }
        if (parse_onoff(a1, &off) && !off)
            wpm = 0;
        else if (!parse_int(a1, &wpm) || wpm < 0 || wpm > 1000) {
            report(mode, where, "error: usage: autotype <wpm 0..1000>|off");
            return -1;
        }
        if (next_tok(&p, extra, sizeof extra)) {
            report(mode, where, "error: usage: autotype <wpm>|off");
            return -1;
        }
        if (exec)
            host_autotype_set((int)wpm);
        return 0;
    }
    if (!strcmp(name, "screenshot")) {
        char path[1024];
        rest_str(p, path, sizeof path);
        if (!path[0]) {
            report(mode, where, "error: usage: screenshot <file.png>");
            return -1;
        }
        if (exec && host_save_screenshot(path) != 0)
            report(mode, where, "error: screenshot: cannot write '%s'", path);
        return 0;
    }
    if (!strcmp(name, "log")) {
        char text[1024];
        rest_str(p, text, sizeof text);
        if (exec)
            host_log("script: %s", text);
        return 0;
    }
    if (!strcmp(name, "expect"))
        return cmd_expect(p, where, mode);
    if (!strcmp(name, "end") || !strcmp(name, "quit") || !strcmp(name, "exit")) {
        if (next_tok(&p, extra, sizeof extra)) {
            report(mode, where, "error: usage: end");
            return -1;
        }
        if (exec)
            H.quit = true;
        return 0;
    }
    report(mode, where, "error: unknown command '%s'", name);
    return -1;
}

/* ------------------------------------------------------------ lines */

/* strip a '#' comment (at line start or after whitespace, outside quotes) and trim */
static void clean_line(const char *in, char *out, size_t n)
{
    bool quoted = false;
    size_t k = 0;
    for (size_t i = 0; in[i] && in[i] != '\n' && in[i] != '\r'; i++) {
        char c = in[i];
        if (c == '"' && (i == 0 || in[i - 1] != '\\'))
            quoted = !quoted;
        if (c == '#' && !quoted && (i == 0 || isspace((unsigned char)in[i - 1])))
            break;
        if (k + 1 < n)
            out[k++] = c;
    }
    out[k] = 0;
    while (k && isspace((unsigned char)out[k - 1]))
        out[--k] = 0;
    const char *s = skip_ws(out);
    if (s != out)
        memmove(out, s, strlen(s) + 1);
}

/*
 * Handle the time prefix of one line (at / wait / sleep / +ms) relative to
 * *cursor, then validate the command and queue it.  When `now` is set
 * (interactive use) a command without a time prefix runs immediately.
 */
static int process_line(const char *raw, const char *where, int64_t *cursor, bool now)
{
    char line[2048], tok[64];
    clean_line(raw, line, sizeof line);
    const char *p = line;
    if (!next_tok(&p, tok, sizeof tok))
        return 0;
    bool prefixed = true;
    int64_t ms;
    const char *cmd = line;
    if (!strcasecmp(tok, "at")) {
        if (!next_tok(&p, tok, sizeof tok) || !parse_ms(tok, &ms)) {
            fprintf(stderr, "%s: error: usage: at <ms> [command]\n", where);
            return -1;
        }
        *cursor = ms;
        cmd = skip_ws(p);
    } else if (!strcasecmp(tok, "wait") || !strcasecmp(tok, "sleep")) {
        char extra[16];
        if (!next_tok(&p, tok, sizeof tok) || !parse_ms(tok, &ms) || next_tok(&p, extra, sizeof extra)) {
            fprintf(stderr, "%s: error: usage: wait <ms>\n", where);
            return -1;
        }
        *cursor += ms;
        return 0;
    } else if (tok[0] == '+') {
        if (!parse_ms(tok + 1, &ms) || !*skip_ws(p)) {
            fprintf(stderr, "%s: error: usage: +<ms> <command>\n", where);
            return -1;
        }
        *cursor += ms;
        cmd = skip_ws(p);
    } else {
        prefixed = false;
    }
    if (!*cmd)
        return 0;
    int64_t dur;
    if (now && !prefixed)
        return run_cmd(cmd, *cursor, where, M_EXEC, &dur);
    if (run_cmd(cmd, *cursor, where, M_CHECK, &dur))
        return -1;
    if (q_insert(*cursor, cmd, where, true))
        return -1;
    *cursor += dur;
    return 0;
}

/* ------------------------------------------------------------ JSON */

enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ };

struct jv {
    int type, line;
    double num;
    char *str;
    struct jv *items;
    char **keys;
    int n;
};

struct jp {
    const char *s;
    int line;
    const char *err;
    int err_line;
};

static void jfree(struct jv *v)
{
    free(v->str);
    for (int i = 0; i < v->n; i++) {
        jfree(&v->items[i]);
        if (v->keys)
            free(v->keys[i]);
    }
    free(v->items);
    free(v->keys);
    memset(v, 0, sizeof *v);
}

static void jws(struct jp *p)
{
    for (;;) {
        char c = *p->s;
        if (c == '\n')
            p->line++;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            p->s++;
        else
            break;
    }
}

static bool jfail(struct jp *p, const char *msg)
{
    if (!p->err) {
        p->err = msg;
        p->err_line = p->line;
    }
    return false;
}

static void put_utf8(char *o, size_t *k, unsigned cp)
{
    if (cp < 0x80) {
        o[(*k)++] = (char)cp;
    } else if (cp < 0x800) {
        o[(*k)++] = (char)(0xC0 | cp >> 6);
        o[(*k)++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        o[(*k)++] = (char)(0xE0 | cp >> 12);
        o[(*k)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[(*k)++] = (char)(0x80 | (cp & 0x3F));
    } else {
        o[(*k)++] = (char)(0xF0 | cp >> 18);
        o[(*k)++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        o[(*k)++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[(*k)++] = (char)(0x80 | (cp & 0x3F));
    }
}

static int hex4(const char *s)
{
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= c - '0';
        else if (c >= 'a' && c <= 'f')
            v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            v |= c - 'A' + 10;
        else
            return -1;
    }
    return v;
}

static bool jstring(struct jp *p, char **out)
{
    p->s++; /* opening quote */
    size_t cap = 64, k = 0;
    char *o = malloc(cap);
    if (!o)
        return jfail(p, "out of memory");
    for (;;) {
        if (k + 8 >= cap) {
            char *n = realloc(o, cap *= 2);
            if (!n) {
                free(o);
                return jfail(p, "out of memory");
            }
            o = n;
        }
        unsigned char c = (unsigned char)*p->s;
        if (!c || c == '\n') {
            free(o);
            return jfail(p, "unterminated string");
        }
        p->s++;
        if (c == '"')
            break;
        if (c < 0x20) {
            free(o);
            return jfail(p, "control character in string");
        }
        if (c != '\\') {
            o[k++] = (char)c;
            continue;
        }
        char e = *p->s++;
        switch (e) {
        case '"': o[k++] = '"'; break;
        case '\\': o[k++] = '\\'; break;
        case '/': o[k++] = '/'; break;
        case 'b': o[k++] = '\b'; break;
        case 'f': o[k++] = '\f'; break;
        case 'n': o[k++] = '\n'; break;
        case 'r': o[k++] = '\r'; break;
        case 't': o[k++] = '\t'; break;
        case 'u': {
            int cp = hex4(p->s);
            if (cp < 0) {
                free(o);
                return jfail(p, "bad \\u escape");
            }
            p->s += 4;
            if (cp >= 0xD800 && cp < 0xDC00 && p->s[0] == '\\' && p->s[1] == 'u') {
                int lo = hex4(p->s + 2);
                if (lo >= 0xDC00 && lo < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    p->s += 6;
                }
            }
            put_utf8(o, &k, (unsigned)cp);
            break;
        }
        default:
            free(o);
            return jfail(p, "bad escape in string");
        }
    }
    o[k] = 0;
    *out = o;
    return true;
}

static bool jvalue(struct jp *p, struct jv *v, int depth);

static bool jpush(struct jp *p, struct jv *v, struct jv *item, char *key)
{
    struct jv *ni = realloc(v->items, sizeof(*ni) * (size_t)(v->n + 1));
    if (!ni)
        return jfail(p, "out of memory");
    v->items = ni;
    if (v->type == J_OBJ) {
        char **nk = realloc(v->keys, sizeof(*nk) * (size_t)(v->n + 1));
        if (!nk)
            return jfail(p, "out of memory");
        v->keys = nk;
        v->keys[v->n] = key;
    }
    v->items[v->n++] = *item;
    return true;
}

static bool jvalue(struct jp *p, struct jv *v, int depth)
{
    memset(v, 0, sizeof *v);
    if (depth > 64)
        return jfail(p, "nesting too deep");
    jws(p);
    v->line = p->line;
    char c = *p->s;
    if (c == '"') {
        v->type = J_STR;
        return jstring(p, &v->str);
    }
    if (c == '[' || c == '{') {
        bool obj = c == '{';
        v->type = obj ? J_OBJ : J_ARR;
        p->s++;
        jws(p);
        if (*p->s == (obj ? '}' : ']')) {
            p->s++;
            return true;
        }
        for (;;) {
            char *key = NULL;
            if (obj) {
                jws(p);
                if (*p->s != '"')
                    return jfail(p, "expected a string key");
                if (!jstring(p, &key))
                    return false;
                jws(p);
                if (*p->s != ':') {
                    free(key);
                    return jfail(p, "expected ':'");
                }
                p->s++;
            }
            struct jv item;
            if (!jvalue(p, &item, depth + 1) || !jpush(p, v, &item, key)) {
                jfree(&item);
                free(key);
                return false;
            }
            jws(p);
            if (*p->s == ',') {
                p->s++;
                continue;
            }
            if (*p->s == (obj ? '}' : ']')) {
                p->s++;
                return true;
            }
            return jfail(p, obj ? "expected ',' or '}'" : "expected ',' or ']'");
        }
    }
    if (c == '-' || isdigit((unsigned char)c)) {
        char *end;
        v->type = J_NUM;
        v->num = strtod(p->s, &end);
        if (end == p->s)
            return jfail(p, "bad number");
        p->s = end;
        return true;
    }
    if (!strncmp(p->s, "true", 4) || !strncmp(p->s, "false", 5)) {
        v->type = J_BOOL;
        v->num = c == 't';
        p->s += c == 't' ? 4 : 5;
        return true;
    }
    if (!strncmp(p->s, "null", 4)) {
        v->type = J_NULL;
        p->s += 4;
        return true;
    }
    return jfail(p, c ? "unexpected character" : "unexpected end of input");
}

static int load_json(const char *path, const char *text)
{
    struct jp p = {text, 1, NULL, 0};
    struct jv root;
    if (!jvalue(&p, &root, 0)) {
        fprintf(stderr, "%s:%d: error: JSON: %s\n", path, p.err_line, p.err);
        jfree(&root);
        return -1;
    }
    jws(&p);
    if (*p.s) {
        fprintf(stderr, "%s:%d: error: JSON: trailing characters after the array\n", path, p.line);
        jfree(&root);
        return -1;
    }
    if (root.type != J_ARR) {
        fprintf(stderr, "%s: error: JSON script must be an array\n", path);
        jfree(&root);
        return -1;
    }
    int errors = 0;
    int64_t cursor = 0;
    for (int i = 0; i < root.n; i++) {
        struct jv *e = &root.items[i];
        char where[200];
        snprintf(where, sizeof where, "%s:%d", path, e->line);
        if (e->type == J_STR) {
            if (process_line(e->str, where, &cursor, false))
                errors++;
            continue;
        }
        if (e->type != J_OBJ) {
            fprintf(stderr, "%s: error: element %d must be a string or an object\n", where, i);
            errors++;
            continue;
        }
        char cmd[2048] = "";
        bool bad = false, have_cmd = false;
        int64_t at = -1, wait = 0;
        for (int k = 0; k < e->n && !bad; k++) {
            const char *key = e->keys[k];
            struct jv *v = &e->items[k];
            if (!strcmp(key, "at") || !strcmp(key, "wait") || !strcmp(key, "after")) {
                if (v->type != J_NUM || v->num < 0) {
                    fprintf(stderr, "%s: error: \"%s\" must be a number of ms >= 0\n", where, key);
                    bad = true;
                } else if (key[0] == 'a' && key[1] == 't') {
                    at = (int64_t)(v->num + 0.5);
                } else {
                    wait += (int64_t)(v->num + 0.5);
                }
            } else if (!strcmp(key, "cmd")) {
                if (v->type != J_STR) {
                    fprintf(stderr, "%s: error: \"cmd\" must be a string\n", where);
                    bad = true;
                } else {
                    char tmp[2048];
                    snprintf(tmp, sizeof tmp, "%s%s", v->str, cmd);
                    snprintf(cmd, sizeof cmd, "%s", tmp);
                    have_cmd = true;
                }
            } else if (!strcmp(key, "args")) {
                if (v->type != J_ARR) {
                    fprintf(stderr, "%s: error: \"args\" must be an array\n", where);
                    bad = true;
                    break;
                }
                for (int a = 0; a < v->n && !bad; a++) {
                    struct jv *x = &v->items[a];
                    char s[1024];
                    if (x->type == J_NUM) {
                        if (x->num == (double)(long long)x->num)
                            snprintf(s, sizeof s, " %lld", (long long)x->num);
                        else
                            snprintf(s, sizeof s, " %g", x->num);
                    } else if (x->type == J_BOOL) {
                        snprintf(s, sizeof s, " %s", x->num ? "on" : "off");
                    } else if (x->type == J_STR) {
                        bool ws = !x->str[0];
                        for (const char *c = x->str; *c; c++)
                            if (isspace((unsigned char)*c) || *c == '#')
                                ws = true;
                        if (ws) {
                            /* quote it, escaping quotes and backslashes */
                            size_t k2 = 0;
                            s[k2++] = ' ';
                            s[k2++] = '"';
                            for (const char *c = x->str; *c && k2 + 4 < sizeof s; c++) {
                                if (*c == '"' || *c == '\\')
                                    s[k2++] = '\\';
                                s[k2++] = *c;
                            }
                            s[k2++] = '"';
                            s[k2] = 0;
                        } else {
                            snprintf(s, sizeof s, " %s", x->str);
                        }
                    } else {
                        fprintf(stderr, "%s: error: \"args\" items must be strings, numbers or booleans\n",
                                where);
                        bad = true;
                        break;
                    }
                    strncat(cmd, s, sizeof cmd - strlen(cmd) - 1);
                }
            } else {
                fprintf(stderr, "%s: error: unknown key \"%s\" (use at, wait, after, cmd, args)\n", where,
                        key);
                bad = true;
            }
        }
        if (!bad && !have_cmd && cmd[0]) {
            fprintf(stderr, "%s: error: \"args\" without \"cmd\"\n", where);
            bad = true;
        }
        if (bad) {
            errors++;
            continue;
        }
        if (at >= 0)
            cursor = at;
        cursor += wait;
        if (have_cmd && process_line(cmd, where, &cursor, false))
            errors++;
    }
    jfree(&root);
    return errors ? -1 : 0;
}

static int load_text(const char *path, char *text)
{
    int errors = 0, lineno = 0;
    int64_t cursor = 0;
    char *s = text;
    while (*s) {
        char *nl = strchr(s, '\n');
        if (nl)
            *nl = 0;
        lineno++;
        char where[200];
        snprintf(where, sizeof where, "%s:%d", path, lineno);
        if (process_line(s, where, &cursor, false))
            errors++;
        if (!nl)
            break;
        s = nl + 1;
    }
    return errors ? -1 : 0;
}

static int cmp_entry(const void *a, const void *b)
{
    const struct entry *x = a, *y = b;
    if (x->t != y->t)
        return x->t < y->t ? -1 : 1;
    return x->seq < y->seq ? -1 : x->seq > y->seq;
}

/* ------------------------------------------------------------ API */

/*
 * Load a script (text, or JSON if the name ends in .json or the first
 * non-blank character is '[').  All lines are validated; on any error
 * nothing from this file is queued and -1 is returned.
 */
int script_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "%s: error: cannot open script\n", path);
        return -1;
    }
    size_t cap = 4096, n = 0;
    char *text = malloc(cap);
    for (;;) {
        if (!text)
            break;
        if (n + 1024 > cap) {
            char *t2 = realloc(text, cap *= 2);
            if (!t2) {
                free(text);
                text = NULL;
                break;
            }
            text = t2;
        }
        size_t r = fread(text + n, 1, cap - n - 1, f);
        n += r;
        if (r == 0)
            break;
    }
    fclose(f);
    if (!text) {
        fprintf(stderr, "%s: error: out of memory\n", path);
        return -1;
    }
    text[n] = 0;
    if (strlen(text) != n) {
        fprintf(stderr, "%s: error: script contains NUL bytes\n", path);
        free(text);
        return -1;
    }
    char *body = text;
    if ((unsigned char)body[0] == 0xEF && (unsigned char)body[1] == 0xBB && (unsigned char)body[2] == 0xBF)
        body += 3;

    size_t plen = strlen(path);
    bool json = (plen >= 5 && !strcasecmp(path + plen - 5, ".json")) || *skip_ws(body) == '[';

    /* parse into the queue; roll back on error */
    int64_t keep_end = end_time;
    long first_seq = seq_ctr;
    int rc = json ? load_json(path, body) : load_text(path, body);
    free(text);
    if (rc) {
        /* drop entries added by this load (they all have seq >= first_seq) */
        int k = qh;
        for (int i = qh; i < qn; i++) {
            if (Q[i].seq >= first_seq)
                free(Q[i].text);
            else
                Q[k++] = Q[i];
        }
        qn = k;
        end_time = keep_end;
        fprintf(stderr, "%s: script not loaded (errors above)\n", path);
        return -1;
    }
    qsort(Q + qh, (size_t)(qn - qh), sizeof *Q, cmp_entry);
    return 0;
}

void script_run_due(int64_t t)
{
    while (qh < qn && Q[qh].t <= t && !H.quit) {
        struct entry e = Q[qh++]; /* copy: running it may insert and move the queue */
        int64_t dur;
        run_cmd(e.text, e.t, e.where, e.checked ? M_EXEC_QUIET : M_EXEC, &dur);
        free(e.text);
    }
    if (qh == qn)
        qh = qn = 0;
}

bool script_done(void)
{
    return qh >= qn;
}

int64_t script_end_time(void)
{
    return end_time;
}

/*
 * Execute one line now at time t (interactive / debug use).  A line with a
 * time prefix ("at 500 tap 3", "+200 release 3") is queued instead.
 */
int script_exec_line(const char *line, int64_t t, const char *where)
{
    int64_t cursor = t;
    return process_line(line, where ? where : "exec", &cursor, true);
}
