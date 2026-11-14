/*
 * Entry points of one simulated MCU DLL (see host/sim_api.h): boot sequence,
 * logging, iterable-section markers and read-back queries.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <stdarg.h>

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/activity.h>
#include <zmk/battery.h>
#include <zmk/usb.h>
#include <zmk/behavior.h>
#include <drivers/behavior.h>

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#define SIM_IS_CENTRAL 1
#include <zmk/keymap.h>
#include <zmk/hid.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/hid_indicators.h>
#if IS_ENABLED(CONFIG_ZMK_WPM)
#include <zmk/wpm.h>
#endif
#else
#define SIM_IS_CENTRAL 0
#include <zmk/split/bluetooth/peripheral.h>
#endif

#include "sim_internal.h"

/* Beedly pet sources are built (nice_epaper/CMakeLists.txt) */
#if IS_ENABLED(CONFIG_NICE_BEETLE_PET) && IS_ENABLED(CONFIG_NICE_EPAPER_ON) &&                     \
    IS_ENABLED(CONFIG_ZMK_DISPLAY) && IS_ENABLED(CONFIG_NICE_VIEW_WIDGET_STATUS)
#define SIM_HAS_PET 1
#else
#define SIM_HAS_PET 0
#endif

/* ----------------------------------------------------- section markers */

SIM_SECTION_MARKERS(struct device, device);
SIM_SECTION_MARKERS(const struct sim_init_entry, sim_init_entry);
SIM_SECTION_MARKERS(const struct zmk_behavior_ref, zmk_behavior_ref);
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_LOCAL_IDS)
SIM_SECTION_MARKERS(const struct zmk_behavior_local_id_map, zmk_behavior_local_id_map);
#endif
/* ZMK's own linker script symbols (event_manager.c) */
const struct zmk_event_type *__event_type_start[0] __attribute__((section(".zsim$event_type_a"), used));
const struct zmk_event_type *__event_type_end[0] __attribute__((section(".zsim$event_type_c"), used));
const struct zmk_event_subscription __event_subscriptions_start[0]
    __attribute__((section(".zsim$event_subscription_a"), used));
const struct zmk_event_subscription __event_subscriptions_end[0]
    __attribute__((section(".zsim$event_subscription_c"), used));

/* ------------------------------------------------------------- logging */

const struct sim_host_callbacks *sim_host;
static struct sim_host_callbacks host_cb;
int sim_log_level = 2;
struct sim_hw_state sim_hw;
static struct sim_init_params init_params;

static void vlog(int level, const char *fmt, va_list ap) {
    if (level > sim_log_level || !sim_host || !sim_host->log) {
        return;
    }
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, ap);
    sim_host->log(sim_host->ctx, SIM_IS_CENTRAL ? SIM_ROLE_CENTRAL : SIM_ROLE_PERIPHERAL, level,
                  buf);
}

void sim_log(int level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(level, fmt, ap);
    va_end(ap);
}

void sim_log_module(int level, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vlog(level, fmt, ap);
    va_end(ap);
}

void sim_log_hexdump(int level, const void *data, size_t len, const char *msg) {
    char buf[256];
    size_t n = 0;
    const uint8_t *d = data;
    for (size_t i = 0; i < len && n + 4 < sizeof(buf); i++) {
        n += snprintf(buf + n, sizeof(buf) - n, "%02x ", d[i]);
    }
    sim_log(level, "%s: %s", msg, buf);
}

void assert_post_action(const char *file, unsigned int line) {
    sim_log(SIM_LOG_ERR, "ASSERT failed at %s:%u", file, line);
}

/* ---------------------------------------------------------------- boot */

extern const struct sim_init_entry _sim_init_entry_list_start[];
extern const struct sim_init_entry _sim_init_entry_list_end[];
extern struct device _device_list_start[];
extern struct device _device_list_end[];

const struct device *device_get_binding(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return NULL;
    }
    for (struct device *d = _device_list_start; d < _device_list_end; d++) {
        if (d->name == name) {
            return device_is_ready(d) ? d : NULL;
        }
    }
    for (struct device *d = _device_list_start; d < _device_list_end; d++) {
        if (d->name && strcmp(d->name, name) == 0) {
            return device_is_ready(d) ? d : NULL;
        }
    }
    return NULL;
}

static int init_cmp(const void *a, const void *b) {
    const struct sim_init_entry *const *x = a, *const *y = b;
    if ((*x)->level != (*y)->level) {
        return (*x)->level - (*y)->level;
    }
    if ((*x)->prio != (*y)->prio) {
        return (*x)->prio - (*y)->prio;
    }
    if ((*x)->order != (*y)->order) {
        return (*x)->order - (*y)->order;
    }
    return (*x < *y) ? -1 : (*x > *y); /* stable: position in section */
}

void sim_run_sys_init(void) {
    size_t n = _sim_init_entry_list_end - _sim_init_entry_list_start;
    const struct sim_init_entry **v = (const struct sim_init_entry **)calloc(n ? n : 1, sizeof(*v));
    for (size_t i = 0; i < n; i++) {
        v[i] = &_sim_init_entry_list_start[i];
    }
    qsort(v, n, sizeof(*v), init_cmp);
    for (size_t i = 0; i < n; i++) {
        const struct sim_init_entry *e = v[i];
        if (e->dev) {
            struct device_state *st = e->dev->state;
            st->init_res = e->dev_init ? e->dev_init(e->dev) : 0;
            st->initialized = true;
            if (st->init_res) {
                sim_log(SIM_LOG_WRN, "device %s init returned %d", e->name, st->init_res);
            }
        } else if (e->sys_init) {
            int r = e->sys_init();
            if (r) {
                sim_log(SIM_LOG_DBG, "SYS_INIT %s returned %d", e->name, r);
            }
        }
        sim_log(SIM_LOG_DBG, "init lvl=%d prio=%d %s", e->level, e->prio, e->name);
    }
    free(v);
}

int zmk_sim_main(void); /* ZMK's app/src/main.c, compiled with -Dmain=zmk_sim_main */

bool sim_display_spi_timing;
void sim_fake_hw_init(const struct sim_init_params *p);
void sim_null_page_install(void);

static int api_init(const struct sim_host_callbacks *cb, const struct sim_init_params *p) {
    host_cb = *cb;
    sim_host = &host_cb;
    init_params = *p;
    sim_log_level = p->log_level;
    sim_display_spi_timing = p->display_spi_timing;
    sim_null_page_install();
    sim_fake_hw_init(p);
    sim_fake_set_usb_boot(p->usb_state);

    /* Zephyr starts the system work queue during POST_KERNEL init */
    k_work_queue_start(&k_sys_work_q, NULL, 0, -1, NULL);
    sim_run_sys_init();
    zmk_sim_main();
    sim_kernel_run_pending();
    return 0;
}

static void api_run_until(int64_t t) { sim_kernel_run_until(t); }
static int64_t api_next_event(void) { return sim_kernel_next_event(); }
static int64_t api_now(void) { return k_uptime_get(); }

/* ------------------------------------------------------ position events */

/* Mirrors physical_layouts.c (local keys) and split/bluetooth/central.c
 * (peripheral keys): queue the event and raise it from the system work queue. */
struct sim_pos_ev {
    uint32_t position;
    uint8_t source;
    bool pressed;
};
K_MSGQ_DEFINE(sim_pos_msgq, sizeof(struct sim_pos_ev), 32, 4);

static void pos_work_cb(struct k_work *w) {
    struct sim_pos_ev ev;
    while (k_msgq_get(&sim_pos_msgq, &ev, K_NO_WAIT) == 0) {
        raise_zmk_position_state_changed((struct zmk_position_state_changed){
            .source = ev.source,
            .state = ev.pressed,
            .position = ev.position,
            .timestamp = k_uptime_get(),
        });
    }
}
K_WORK_DEFINE(sim_pos_work, pos_work_cb);

static void api_position_event(uint32_t position, bool pressed, uint8_t source) {
    struct sim_pos_ev ev = {.position = position, .source = source, .pressed = pressed};
    if (k_msgq_put(&sim_pos_msgq, &ev, K_NO_WAIT) != 0) {
        sim_log(SIM_LOG_WRN, "position event queue full, dropping event");
    }
    k_work_submit(&sim_pos_work);
}

/* implemented in fake_hw.c */
void sim_fake_set_battery(uint8_t level);
void sim_fake_set_usb(int state);
void sim_fake_set_split_connected(bool c);
void sim_fake_ble_select(int idx);
void sim_fake_ble_host_connected(int idx, bool c);
void sim_fake_ble_bonded(int idx, bool b);
void sim_fake_hid_indicators(uint8_t leds);
int64_t sim_fake_query(int what, bool *handled);
size_t sim_lvgl_heap_used(void);
size_t sim_lvgl_heap_peak(void);
extern uint32_t sim_display_writes;

static int api_invoke_behavior(const char *dev, uint32_t p1, uint32_t p2, uint32_t position,
                               bool pressed) {
    struct zmk_behavior_binding binding = {.behavior_dev = dev, .param1 = p1, .param2 = p2};
    struct zmk_behavior_binding_event ev = {
        .position = position,
        .timestamp = k_uptime_get(),
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };
    const struct device *behavior = zmk_behavior_get_binding(dev);
    if (!behavior) {
        sim_log(SIM_LOG_WRN, "invoke_behavior: no behavior %s on this half", dev);
        return -ENODEV;
    }
    return pressed ? behavior_keymap_binding_pressed(&binding, ev)
                   : behavior_keymap_binding_released(&binding, ev);
}

static int api_select_transport(int transport) {
#if SIM_IS_CENTRAL
    return zmk_endpoint_set_preferred_transport(transport == 0 ? ZMK_TRANSPORT_USB : ZMK_TRANSPORT_BLE);
#else
    return -ENOTSUP;
#endif
}

static int64_t api_query(int what) {
    bool handled = false;
    int64_t v = sim_fake_query(what, &handled);
    if (handled) {
        return v;
    }
    switch (what) {
#if SIM_IS_CENTRAL
    case SIM_Q_HIGHEST_LAYER:
        return zmk_keymap_highest_layer_active();
    case SIM_Q_LAYER_STATE:
        return zmk_keymap_layer_state();
#if IS_ENABLED(CONFIG_ZMK_WPM)
    case SIM_Q_WPM_ZMK:
        return zmk_wpm_get_state();
#endif
#if defined(CONFIG_NICE_BEETLE_LUNA_WALK_WPM) && IS_ENABLED(CONFIG_NICE_VIEW_WIDGET_STATUS) && IS_ENABLED(CONFIG_ZMK_DISPLAY)
    /* the WPM the central screen shows (nice_epaper widgets/wpm_counter.c, compiled with the
     * central status screen; its Kconfig block defines NICE_BEETLE_LUNA_WALK_WPM) */
    case SIM_Q_WPM: {
        extern uint8_t nice_beetle_wpm_get_state(void);
        return nice_beetle_wpm_get_state();
    }
#elif IS_ENABLED(CONFIG_ZMK_WPM)
    case SIM_Q_WPM:
        return zmk_wpm_get_state();
#endif
    case SIM_Q_EXPLICIT_MODS:
        return zmk_hid_get_explicit_mods();
    case SIM_Q_ENDPOINT_TRANSPORT: {
        /* host API: 0 USB, 1 BLE, -1 none (ZMK main added ZMK_TRANSPORT_NONE = 0) */
        enum zmk_transport t = zmk_endpoint_get_selected().transport;
        return t == ZMK_TRANSPORT_USB ? 0 : t == ZMK_TRANSPORT_BLE ? 1 : -1;
    }
#endif
    case SIM_Q_BATTERY:
        return zmk_battery_state_of_charge();
    case SIM_Q_DISPLAY_WRITES:
        return sim_display_writes;
    case SIM_Q_LVGL_HEAP_USED:
        return (int64_t)sim_lvgl_heap_used();
    case SIM_Q_LVGL_HEAP_PEAK:
        return (int64_t)sim_lvgl_heap_peak();
    case SIM_Q_ACTIVITY_STATE:
        return zmk_activity_get_state();
    case SIM_Q_REBOOT_REQUESTED: {
        extern int sim_reboot_requested;
        return sim_reboot_requested;
    }
    case SIM_Q_PET_SAVES: {
        extern uint32_t sim_settings_beedly_saves;
        return sim_settings_beedly_saves;
    }
#if SIM_HAS_PET && SIM_IS_CENTRAL
    /* widgets/beedly_menu.h */
    case SIM_Q_PET_MENU_OPEN: {
        extern bool beedly_menu_is_open(void);
        return beedly_menu_is_open();
    }
    case SIM_Q_PET_MENU_CURSOR: {
        extern uint8_t beedly_menu_cursor(void);
        return beedly_menu_cursor();
    }
#elif SIM_HAS_PET
    /* widgets/beedly_pet.h */
    case SIM_Q_PET_HEARTS: {
        extern uint8_t beedly_pet_hearts(void);
        return beedly_pet_hearts();
    }
    case SIM_Q_PET_FOOD: {
        extern uint8_t beedly_pet_food(void);
        return beedly_pet_food();
    }
    case SIM_Q_PET_XP: {
        extern uint16_t beedly_pet_xp(void);
        return beedly_pet_xp();
    }
    case SIM_Q_PET_LEVEL: {
        extern uint8_t beedly_pet_level(void);
        return beedly_pet_level();
    }
    case SIM_Q_PET_DIRTY: {
        extern bool beedly_pet_dirty(void);
        return beedly_pet_dirty();
    }
    case SIM_Q_PET_ANIM: {
        extern bool beedly_pet_animating(void);
        return beedly_pet_animating();
    }
#endif
    default:
        return -1;
    }
}

static const struct sim_mcu_api api = {
    .version = SIM_API_VERSION,
    .role = SIM_IS_CENTRAL ? SIM_ROLE_CENTRAL : SIM_ROLE_PERIPHERAL,
    .init = api_init,
    .run_until = api_run_until,
    .next_event = api_next_event,
    .now = api_now,
    .position_event = api_position_event,
    .set_battery = sim_fake_set_battery,
    .set_usb = sim_fake_set_usb,
    .set_split_connected = sim_fake_set_split_connected,
    .ble_select_profile = sim_fake_ble_select,
    .ble_set_host_connected = sim_fake_ble_host_connected,
    .ble_set_bonded = sim_fake_ble_bonded,
    .hid_indicators_report = sim_fake_hid_indicators,
    .split_rx = sim_split_rx,
    .invoke_behavior = api_invoke_behavior,
    .query = api_query,
    .select_transport = api_select_transport,
};

__declspec(dllexport) const struct sim_mcu_api *sim_mcu_get_api(void) { return &api; }
