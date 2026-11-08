/*
 * Simulated hardware / radio layer.  Everything here replaces code that talks
 * to real silicon or the BLE/USB stacks; the ZMK logic on top of it is real.
 *
 *   USB          : usb_enable() stand-in for Zephyr's USB device stack; ZMK's
 *                  real usb.c runs on top.  usb_hid.c is replaced (reports go
 *                  to the host model).
 *   Battery      : sensor driver bound to the chosen zmk,battery node (like
 *                  nice!nano's vbatt); ZMK's real battery.c samples it on its
 *                  normal schedule.
 *   BLE (central): replaces ble.c + hog.c with a model of profiles, bonds and
 *                  host connections that raises the same events ble.c does.
 *   Split        : a split *transport* (the layer below ZMK's real
 *                  split/central.c and split/peripheral.c); packets travel via
 *                  the host.  bluetooth/peripheral.c's connection status API
 *                  and event are reproduced.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/bluetooth/addr.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/usb.h>
#include <zmk/battery.h>

#include "sim_internal.h"

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#define SIM_IS_CENTRAL 1
#else
#define SIM_IS_CENTRAL 0
#endif

/* Bluetooth address constants (Zephyr subsys/bluetooth/common/addr.c) */
const bt_addr_le_t bt_addr_le_any = {0, {{0, 0, 0, 0, 0, 0}}};
const bt_addr_le_t bt_addr_le_none = {0, {{0xff, 0xff, 0xff, 0xff, 0xff, 0xff}}};

/* ================================================================ USB */

static usb_dc_status_callback usb_cb;
static int usb_state = SIM_USB_NONE;

int usb_enable(usb_dc_status_callback status_cb) {
    usb_cb = status_cb;
    return 0;
}

int usb_disable(void) { return 0; }
int usb_wakeup_request(void) { return 0; }

void sim_fake_set_usb(int state) {
    if (state == usb_state) {
        return;
    }
    int old = usb_state;
    usb_state = state;
    if (!usb_cb) {
        return;
    }
    /* Sequence of device-controller callbacks a real plug/enumerate produces */
    if (state == SIM_USB_NONE) {
        usb_cb(USB_DC_DISCONNECTED, NULL);
    } else if (state == SIM_USB_POWERED) {
        usb_cb(old == SIM_USB_HID ? USB_DC_DISCONNECTED : USB_DC_CONNECTED, NULL);
        if (old == SIM_USB_HID) {
            usb_cb(USB_DC_CONNECTED, NULL); /* charger-only: powered, never configured */
        }
    } else {
        if (old == SIM_USB_NONE) {
            usb_cb(USB_DC_CONNECTED, NULL);
        }
        usb_cb(USB_DC_RESET, NULL);
        usb_cb(USB_DC_CONFIGURED, NULL);
    }
}

/* ============================================================ battery */

#if DT_HAS_CHOSEN(zmk_battery)
static int sim_batt_fetch(const struct device *dev, enum sensor_channel chan) { return 0; }
static int sim_batt_get(const struct device *dev, enum sensor_channel chan,
                        struct sensor_value *val) {
    if (chan == SENSOR_CHAN_GAUGE_STATE_OF_CHARGE) {
        val->val1 = sim_hw.battery_level;
        val->val2 = 0;
        return 0;
    }
    if (chan == SENSOR_CHAN_VOLTAGE || chan == SENSOR_CHAN_GAUGE_VOLTAGE) {
        /* inverse of battery.c's lithium_ion_mv_to_pct() */
        int mv = sim_hw.battery_level >= 100 ? 4200 : (sim_hw.battery_level + 459) * 15 / 2;
        val->val1 = mv / 1000;
        val->val2 = (mv % 1000) * 1000;
        return 0;
    }
    return -ENOTSUP;
}
static const struct sensor_driver_api sim_batt_api = {
    .sample_fetch = sim_batt_fetch,
    .channel_get = sim_batt_get,
};
static int sim_batt_init(const struct device *dev) { return 0; }
DEVICE_DT_DEFINE(DT_CHOSEN(zmk_battery), sim_batt_init, NULL, NULL, NULL, POST_KERNEL,
                 CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &sim_batt_api);
#endif

void sim_fake_set_battery(uint8_t level) { sim_hw.battery_level = MIN(level, 100); }

/* ================================================================ BLE */

#if SIM_IS_CENTRAL && IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#include <zmk/hog.h>
#include <zmk/hid.h>
#include <zmk/endpoints.h>
#include <zmk/hid_indicators.h>
#include <zmk/events/ble_active_profile_changed.h>

static struct zmk_ble_profile profiles[ZMK_BLE_PROFILE_COUNT];
static bool prof_connected[ZMK_BLE_PROFILE_COUNT];
static bool host_present[ZMK_BLE_PROFILE_COUNT];
static uint8_t active_profile;

static void raise_profile_changed_event(void) {
    raise_zmk_ble_active_profile_changed((struct zmk_ble_active_profile_changed){
        .index = active_profile, .profile = &profiles[active_profile]});
}
static void raise_profile_changed_event_callback(struct k_work *work) {
    raise_profile_changed_event();
}
K_WORK_DEFINE(raise_profile_changed_event_work, raise_profile_changed_event_callback);

static void set_addr(uint8_t i, bool bonded) {
    if (bonded) {
        bt_addr_le_t a = {.type = BT_ADDR_LE_RANDOM, .a = {.val = {0xC0, 0x5E, 0x11, 0, 0, i + 1}}};
        bt_addr_le_copy(&profiles[i].peer, &a);
        snprintf(profiles[i].name, sizeof(profiles[i].name), "host%u", i);
    } else {
        bt_addr_le_copy(&profiles[i].peer, BT_ADDR_LE_ANY);
        profiles[i].name[0] = 0;
    }
}

bool zmk_ble_profile_is_open(uint8_t index) {
    if (index >= ZMK_BLE_PROFILE_COUNT) {
        return false;
    }
    return !bt_addr_le_cmp(&profiles[index].peer, BT_ADDR_LE_ANY);
}
bool zmk_ble_active_profile_is_open(void) { return zmk_ble_profile_is_open(active_profile); }
bool zmk_ble_profile_is_connected(uint8_t index) {
    return index < ZMK_BLE_PROFILE_COUNT && prof_connected[index];
}
bool zmk_ble_active_profile_is_connected(void) {
    return zmk_ble_profile_is_connected(active_profile);
}
int zmk_ble_active_profile_index(void) { return active_profile; }
int zmk_ble_profile_index(const bt_addr_le_t *addr) {
    for (int i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
        if (bt_addr_le_cmp(addr, &profiles[i].peer) == 0) {
            return i;
        }
    }
    return -ENODEV;
}
bt_addr_le_t *zmk_ble_profile_address(uint8_t index) {
    return index < ZMK_BLE_PROFILE_COUNT ? &profiles[index].peer : (bt_addr_le_t *)BT_ADDR_LE_NONE;
}
bt_addr_le_t *zmk_ble_active_profile_addr(void) { return &profiles[active_profile].peer; }
struct bt_conn *zmk_ble_active_profile_conn(void) { return NULL; }
char *zmk_ble_active_profile_name(void) { return profiles[active_profile].name; }
int zmk_ble_set_device_name(char *name) { return 0; }
int zmk_ble_unpair_all(void) {
    zmk_ble_clear_all_bonds();
    return 0;
}
int zmk_ble_put_peripheral_addr(const bt_addr_le_t *addr) { return 0; }

/* host model: a present host for an (un)bonded active profile (re)connects */
static void host_model_cb(struct k_work *work);
K_WORK_DELAYABLE_DEFINE(host_model_work, host_model_cb);

static void update_advertising(void) {
    /* Real ble.c starts (directed/undirected) advertising here; a host that is
     * present then connects (and pairs if the profile is open) a little later. */
    k_work_reschedule(&host_model_work, K_MSEC(zmk_ble_active_profile_is_open() ? 1500 : 400));
}

static void host_model_cb(struct k_work *work) {
    uint8_t i = active_profile;
    if (!host_present[i] || prof_connected[i]) {
        return;
    }
    if (zmk_ble_profile_is_open(i)) {
        set_addr(i, true); /* pairing completes -> bond stored */
        sim_log(SIM_LOG_INF, "BLE: host paired on profile %u", i);
    }
    prof_connected[i] = true;
    sim_log(SIM_LOG_INF, "BLE: profile %u connected", i);
    k_work_submit(&raise_profile_changed_event_work); /* as ble.c connected() */
}

int zmk_ble_prof_select(uint8_t index) {
    if (index >= ZMK_BLE_PROFILE_COUNT) {
        return -ERANGE;
    }
    if (active_profile == index) {
        return 0;
    }
    active_profile = index;
    update_advertising();
    raise_profile_changed_event();
    return 0;
}
int zmk_ble_prof_next(void) { return zmk_ble_prof_select((active_profile + 1) % ZMK_BLE_PROFILE_COUNT); }
int zmk_ble_prof_prev(void) {
    return zmk_ble_prof_select((active_profile + ZMK_BLE_PROFILE_COUNT - 1) % ZMK_BLE_PROFILE_COUNT);
}
int zmk_ble_prof_disconnect(uint8_t index) {
    if (index >= ZMK_BLE_PROFILE_COUNT) {
        return -ERANGE;
    }
    if (zmk_ble_profile_is_open(index)) {
        return -ENODEV;
    }
    if (prof_connected[index]) {
        prof_connected[index] = false;
        k_work_submit(&raise_profile_changed_event_work);
    }
    return 0;
}

static void clear_profile_bond(uint8_t i) {
    if (!zmk_ble_profile_is_open(i)) {
        bool was = prof_connected[i];
        prof_connected[i] = false;
        set_addr(i, false);
        k_work_submit(&raise_profile_changed_event_work); /* set_profile_address() */
        if (was) {
            sim_log(SIM_LOG_INF, "BLE: profile %u unpaired", i);
        }
    }
}
void zmk_ble_clear_bonds(void) {
    clear_profile_bond(active_profile);
    update_advertising();
}
void zmk_ble_clear_all_bonds(void) {
    for (int i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
        clear_profile_bond(i);
    }
    zmk_ble_prof_select(0);
    update_advertising();
}

void sim_fake_ble_select(int idx) { zmk_ble_prof_select(idx); }
void sim_fake_ble_host_connected(int idx, bool c) {
    if (idx < 0 || idx >= ZMK_BLE_PROFILE_COUNT) {
        return;
    }
    host_present[idx] = c;
    if (!c && prof_connected[idx]) {
        prof_connected[idx] = false;
        k_work_submit(&raise_profile_changed_event_work); /* ble.c disconnected() */
        if (idx == active_profile) {
            update_advertising();
        }
    } else if (c && idx == active_profile) {
        update_advertising();
    }
}
void sim_fake_ble_bonded(int idx, bool b) {
    if (idx < 0 || idx >= ZMK_BLE_PROFILE_COUNT) {
        return;
    }
    if (!b) {
        clear_profile_bond(idx);
    } else if (zmk_ble_profile_is_open(idx)) {
        set_addr(idx, true);
        k_work_submit(&raise_profile_changed_event_work);
    }
}

static void ble_init_state(const struct sim_init_params *p) {
    for (int i = 0; i < ZMK_BLE_PROFILE_COUNT; i++) {
        set_addr(i, false);
    }
    /* default: profile 0 paired with a host that is in range */
    set_addr(0, true);
    host_present[0] = true;
}

static int sim_ble_init(void) {
    update_advertising();
    return 0;
}
SYS_INIT(sim_ble_init, APPLICATION, CONFIG_ZMK_BLE_INIT_PRIORITY);

/* hog.c replacement: reports go to the host model if the active profile is connected */
int zmk_hog_send_keyboard_report(struct zmk_hid_keyboard_report_body *body) {
    if (!zmk_ble_active_profile_is_connected()) {
        return -ENOTCONN;
    }
    if (sim_host->hid_report) {
        sim_host->hid_report(sim_host->ctx, 1, HID_USAGE_KEY, (const uint8_t *)body, sizeof(*body));
    }
    return 0;
}
int zmk_hog_send_consumer_report(struct zmk_hid_consumer_report_body *body) {
    if (!zmk_ble_active_profile_is_connected()) {
        return -ENOTCONN;
    }
    if (sim_host->hid_report) {
        sim_host->hid_report(sim_host->ctx, 1, HID_USAGE_CONSUMER, (const uint8_t *)body,
                             sizeof(*body));
    }
    return 0;
}
#if IS_ENABLED(CONFIG_ZMK_POINTING)
int zmk_hog_send_mouse_report(struct zmk_hid_mouse_report_body *body) { return 0; }
#endif
#else
static void ble_init_state(const struct sim_init_params *p) {}
void sim_fake_ble_select(int idx) {}
void sim_fake_ble_host_connected(int idx, bool c) {}
void sim_fake_ble_bonded(int idx, bool b) {}
#endif /* central BLE */

/* ============================================================ USB HID */

#if SIM_IS_CENTRAL && IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb_hid.h>
#include <zmk/hid.h>

static int usb_send(uint8_t page, const uint8_t *r, size_t len) {
    switch (zmk_usb_get_status()) {
    case USB_DC_ERROR:
    case USB_DC_RESET:
    case USB_DC_DISCONNECTED:
    case USB_DC_UNKNOWN:
        return -ENODEV;
    default:
        break;
    }
    if (sim_host->hid_report) {
        sim_host->hid_report(sim_host->ctx, 0, page, r, len);
    }
    return 0;
}
int zmk_usb_hid_send_keyboard_report(void) {
    struct zmk_hid_keyboard_report *r = zmk_hid_get_keyboard_report();
    return usb_send(HID_USAGE_KEY, (const uint8_t *)&r->body, sizeof(r->body));
}
int zmk_usb_hid_send_consumer_report(void) {
    struct zmk_hid_consumer_report *r = zmk_hid_get_consumer_report();
    return usb_send(HID_USAGE_CONSUMER, (const uint8_t *)&r->body, sizeof(r->body));
}
#if IS_ENABLED(CONFIG_ZMK_POINTING)
int zmk_usb_hid_send_mouse_report(void) { return 0; }
#endif
void zmk_usb_hid_set_protocol(uint8_t protocol) {}
#endif

/* ===================================================== HID indicators */

#if SIM_IS_CENTRAL && IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
#include <zmk/hid_indicators.h>
#include <zmk/endpoints.h>
void sim_fake_hid_indicators(uint8_t leds) {
    /* What usb_hid.c set_report_cb / hog.c write_hids_leds do with an LED output report */
    struct zmk_hid_led_report_body body = {.leds = leds};
    zmk_hid_indicators_process_report(&body, zmk_endpoint_get_selected());
}
#else
void sim_fake_hid_indicators(uint8_t leds) {}
#endif

/* ============================================================== split */

#if IS_ENABLED(CONFIG_ZMK_SPLIT)
#if SIM_IS_CENTRAL
#include <zmk/split/transport/central.h>

SIM_SECTION_MARKERS(const struct zmk_split_transport_central, zmk_split_transport_central);
static zmk_split_transport_central_status_changed_cb_t central_status_cb;
static const struct zmk_split_transport_central_api sim_central_api;
ZMK_SPLIT_TRANSPORT_CENTRAL_REGISTER(sim_split_central, &sim_central_api, 0);

static int c_send_command(uint8_t source, struct zmk_split_transport_central_command cmd) {
    if (!sim_hw.split_connected) {
        return -ENOTCONN;
    }
    sim_host->split_tx(sim_host->ctx, SIM_ROLE_CENTRAL, &cmd, sizeof(cmd));
    return 0;
}
static int c_sources(uint8_t *sources) {
    if (!sim_hw.split_connected) {
        return 0;
    }
    sources[0] = 0;
    return 1;
}
static int c_set_enabled(bool en) { return 0; }
static struct zmk_split_transport_status c_status(void) {
    return (struct zmk_split_transport_status){
        .enabled = true,
        .available = true,
        .connections = sim_hw.split_connected ? ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED
                                              : ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED,
    };
}
static int c_set_cb(zmk_split_transport_central_status_changed_cb_t cb) {
    central_status_cb = cb;
    return 0;
}
static const struct zmk_split_transport_central_api sim_central_api = {
    .send_command = c_send_command,
    .get_available_source_ids = c_sources,
    .set_enabled = c_set_enabled,
    .get_status = c_status,
    .set_status_callback = c_set_cb,
};

/* like bluetooth/central.c: received notifications are queued and handled from the sys work queue */
K_MSGQ_DEFINE(sim_split_rx_q, sizeof(struct zmk_split_transport_peripheral_event), 16, 4);
static void split_rx_work_cb(struct k_work *w) {
    struct zmk_split_transport_peripheral_event ev;
    while (k_msgq_get(&sim_split_rx_q, &ev, K_NO_WAIT) == 0) {
        zmk_split_transport_central_peripheral_event_handler(&sim_split_central, 0, ev);
    }
}
K_WORK_DEFINE(sim_split_rx_work, split_rx_work_cb);

void sim_split_rx(const void *data, size_t len) {
    if (len != sizeof(struct zmk_split_transport_peripheral_event)) {
        sim_log(SIM_LOG_ERR, "split rx: bad packet size %u", (unsigned)len);
        return;
    }
    k_msgq_put(&sim_split_rx_q, data, K_NO_WAIT);
    k_work_submit(&sim_split_rx_work);
}

void sim_fake_set_split_connected(bool c) {
    if (c == sim_hw.split_connected) {
        return;
    }
    sim_hw.split_connected = c;
    if (central_status_cb) {
        central_status_cb(&sim_split_central, c_status());
    }
}
#else /* peripheral */
#include <zmk/split/transport/peripheral.h>
#include <zmk/split/bluetooth/peripheral.h>
#include <zmk/events/split_peripheral_status_changed.h>

SIM_SECTION_MARKERS(const struct zmk_split_transport_peripheral, zmk_split_transport_peripheral);
static zmk_split_transport_peripheral_status_changed_cb_t periph_status_cb;
static const struct zmk_split_transport_peripheral_api sim_periph_api;
ZMK_SPLIT_TRANSPORT_PERIPHERAL_REGISTER(sim_split_peripheral, &sim_periph_api, 0);

static int p_report(const struct zmk_split_transport_peripheral_event *ev) {
    if (ev->type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT) {
        /* split/bluetooth/service.c: "The BLE transport uses standard BAS service
         * for propagation, so just return success here."  (BAS to the central is
         * only read with ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING, which is off.) */
        return 0;
    }
    if (!sim_hw.split_connected) {
        return -ENOTCONN;
    }
    sim_host->split_tx(sim_host->ctx, SIM_ROLE_PERIPHERAL, ev, sizeof(*ev));
    return 0;
}
static int p_set_enabled(bool en) { return 0; }
static struct zmk_split_transport_status p_status(void) {
    return (struct zmk_split_transport_status){
        .enabled = true,
        .available = true,
        .connections = sim_hw.split_connected ? ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED
                                              : ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED,
    };
}
static int p_set_cb(zmk_split_transport_peripheral_status_changed_cb_t cb) {
    periph_status_cb = cb;
    return 0;
}
static const struct zmk_split_transport_peripheral_api sim_periph_api = {
    .report_event = p_report,
    .set_enabled = p_set_enabled,
    .get_status = p_status,
    .set_status_callback = p_set_cb,
};

bool zmk_split_bt_peripheral_is_connected(void) { return sim_hw.split_connected; }
bool zmk_split_bt_peripheral_is_bonded(void) { return true; }

K_MSGQ_DEFINE(sim_split_cmd_q, sizeof(struct zmk_split_transport_central_command), 8, 4);
static void split_cmd_work_cb(struct k_work *w) {
    struct zmk_split_transport_central_command cmd;
    while (k_msgq_get(&sim_split_cmd_q, &cmd, K_NO_WAIT) == 0) {
        zmk_split_transport_peripheral_command_handler(&sim_split_peripheral, cmd);
    }
}
K_WORK_DEFINE(sim_split_cmd_work, split_cmd_work_cb);

void sim_split_rx(const void *data, size_t len) {
    if (len != sizeof(struct zmk_split_transport_central_command)) {
        sim_log(SIM_LOG_ERR, "split rx: bad command size %u", (unsigned)len);
        return;
    }
    k_msgq_put(&sim_split_cmd_q, data, K_NO_WAIT);
    k_work_submit(&sim_split_cmd_work);
}

/* bluetooth/peripheral.c raises this from its connected()/disconnected() callbacks */
static void raise_conn_work_cb(struct k_work *w) {
    raise_zmk_split_peripheral_status_changed(
        (struct zmk_split_peripheral_status_changed){.connected = sim_hw.split_connected});
}
K_WORK_DEFINE(sim_split_conn_work, raise_conn_work_cb);

void sim_fake_set_split_connected(bool c) {
    if (c == sim_hw.split_connected) {
        return;
    }
    sim_hw.split_connected = c;
    k_work_submit(&sim_split_conn_work);
    if (periph_status_cb) {
        periph_status_cb(&sim_split_peripheral, p_status());
    }
}
#endif
#else
void sim_fake_set_split_connected(bool c) { sim_hw.split_connected = c; }
void sim_split_rx(const void *data, size_t len) {}
#endif /* CONFIG_ZMK_SPLIT */

/* ===================================================== physical layouts */

#include <zmk/matrix.h>
static uint32_t identity_map[ZMK_KEYMAP_LEN];
int zmk_physical_layouts_get_selected_to_stock_position_map(uint32_t const **map) {
    for (uint32_t i = 0; i < ZMK_KEYMAP_LEN; i++) {
        identity_map[i] = i;
    }
    *map = identity_map;
    return ZMK_KEYMAP_LEN;
}
int zmk_physical_layouts_get_selected(void) { return 0; }
int zmk_physical_layouts_select(uint8_t index) { return index == 0 ? 0 : -EINVAL; }

/* ====================================================== init / queries */

void sim_fake_hw_init(const struct sim_init_params *p) {
    sim_hw.battery_level = p->battery_level;
    sim_hw.split_connected = p->split_connected;
    ble_init_state(p);
}

/* USB plug state requested before boot is applied once usb.c registered its callback */
static int sim_usb_boot_state;
static int sim_usb_apply_boot(void) {
    if (sim_usb_boot_state) {
        int s = sim_usb_boot_state;
        usb_state = SIM_USB_NONE;
        sim_fake_set_usb(s);
    }
    return 0;
}
SYS_INIT(sim_usb_apply_boot, APPLICATION, 99);
void sim_fake_set_usb_boot(int s) { sim_usb_boot_state = s; }

int64_t sim_fake_query(int what, bool *handled) {
    *handled = true;
    switch (what) {
    case SIM_Q_USB_STATE:
        return usb_state;
    case SIM_Q_SPLIT_CONNECTED:
        return sim_hw.split_connected;
#if SIM_IS_CENTRAL && IS_ENABLED(CONFIG_ZMK_BLE)
    case SIM_Q_ACTIVE_PROFILE:
        return active_profile;
    case SIM_Q_PROFILE_CONNECTED:
        return prof_connected[active_profile];
    case SIM_Q_PROFILE_BONDED:
        return !zmk_ble_profile_is_open(active_profile);
#endif
#if SIM_IS_CENTRAL && IS_ENABLED(CONFIG_ZMK_HID_INDICATORS)
    case SIM_Q_HID_INDICATORS:
        return zmk_hid_indicators_get_current_profile();
#endif
    default:
        *handled = false;
        return 0;
    }
}
