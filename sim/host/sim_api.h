/*
 * Interface between the simulator host (sim.exe: window, input, panel
 * model, scripting) and one simulated MCU (central.dll / peripheral.dll: the
 * real firmware + ZMK + LVGL compiled against the shim kernel).
 *
 * Each DLL is a separate "chip": its own LVGL instance, own kernel clock, own
 * globals.  The host advances both clocks in lock-step.
 */
#ifndef SIM_API_H
#define SIM_API_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define SIM_API_VERSION 4

enum sim_role { SIM_ROLE_CENTRAL = 0, SIM_ROLE_PERIPHERAL = 1 };

enum sim_usb_state { SIM_USB_NONE = 0, SIM_USB_POWERED = 1, SIM_USB_HID = 2 };

/* Things the host can read back (for tests / the status line). */
enum sim_query {
    SIM_Q_HIGHEST_LAYER,
    SIM_Q_LAYER_STATE,
    SIM_Q_WPM,
    SIM_Q_EXPLICIT_MODS,
    SIM_Q_BATTERY,
    SIM_Q_USB_STATE,
    SIM_Q_ACTIVE_PROFILE,
    SIM_Q_PROFILE_CONNECTED,
    SIM_Q_PROFILE_BONDED,
    SIM_Q_ENDPOINT_TRANSPORT,
    SIM_Q_SPLIT_CONNECTED,
    SIM_Q_HID_INDICATORS,
    SIM_Q_DISPLAY_WRITES,
    SIM_Q_LVGL_HEAP_USED,
    SIM_Q_LVGL_HEAP_PEAK,
    SIM_Q_ACTIVITY_STATE,
    SIM_Q_REBOOT_REQUESTED, /* >= 0: firmware called sys_reboot(type); reload the DLL */
    SIM_Q_WPM_ZMK,          /* ZMK's own counter (SIM_Q_WPM = what the screen shows) */
};

struct sim_host_callbacks {
    void *ctx;
    /* A log line from the firmware (LOG_*) or the shim. */
    void (*log)(void *ctx, int role, int level, const char *msg);
    /* The firmware's display driver received a write: rows y0..y1 changed.
     * fb is the full panel image after the write (w*h bytes, 1 = white/lit
     * pixel, i.e. Zephyr PIXEL_FORMAT_MONO01 semantics as used by ls0xx).
     * start_us is when the SPI transfer of this write starts (MCU uptime in
     * microseconds) and xfer_us how long it takes; both are 0-length/"now"
     * with display_spi_timing off (ideal, instantaneous panel). */
    void (*display_write)(void *ctx, int role, int64_t start_us, int y0, int y1, const uint8_t *fb,
                          int w, int h, int xfer_us);
    /* Central only: a HID report was sent to the host over `transport` (0 USB, 1 BLE). */
    void (*hid_report)(void *ctx, int transport, uint8_t usage_page, const uint8_t *report,
                       size_t len);
    /* Split link: the half's ZMK split transport wants to send a packet to the
     * other half (peripheral -> central: zmk_split_transport_peripheral_event,
     * central -> peripheral: zmk_split_transport_central_command).  The host
     * delivers it with split_rx() on the other DLL. */
    void (*split_tx)(void *ctx, int role, const void *data, size_t len);
};

struct sim_init_params {
    /* Sharp memory LCD (ls0xx) bus timing: when set, each display write
     * occupies the display thread for the time Zephyr's ls0xx driver needs to
     * shift it out over SPI at the overlay's spi-max-frequency (write command
     * byte + per line: address byte, 20 data bytes, dummy byte + trailer byte).
     * 0 = instantaneous. */
    bool display_spi_timing;
    int log_level;
    uint8_t battery_level;
    int usb_state;
    bool split_connected;
};

struct sim_mcu_api {
    int version;
    int role;
    int (*init)(const struct sim_host_callbacks *cb, const struct sim_init_params *params);
    void (*run_until)(int64_t t_ms);
    int64_t (*next_event)(void);
    int64_t (*now)(void);

    /* Matrix position event as produced by the kscan/physical-layout layer
     * (central: source 0xFF = local, 0 = from the peripheral over split). */
    void (*position_event)(uint32_t position, bool pressed, uint8_t source);
    void (*set_battery)(uint8_t level);
    void (*set_usb)(int state);
    void (*set_split_connected)(bool connected);

    /* Central only: Bluetooth host model. */
    void (*ble_select_profile)(int index);
    void (*ble_set_host_connected)(int index, bool connected);
    void (*ble_set_bonded)(int index, bool bonded);
    /* Central only: LED report from the host (num/caps/scroll lock...). */
    void (*hid_indicators_report)(uint8_t leds);
    /* A split packet from the other half (see split_tx). */
    void (*split_rx)(const void *data, size_t len);
    /* Run a behaviour by device name (debug helper). */
    int (*invoke_behavior)(const char *dev, uint32_t p1, uint32_t p2, uint32_t position,
                           bool pressed);

    int64_t (*query)(int what);

    /* Central only: what &out OUT_USB / OUT_BLE do (zmk_endpoint_set_preferred_transport);
     * transport 0 = USB, 1 = BLE. Returns the ZMK result. */
    int (*select_transport)(int transport);
};

typedef const struct sim_mcu_api *(*sim_mcu_get_api_fn)(void);

#endif
