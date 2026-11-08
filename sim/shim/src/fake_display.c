/*
 * Stand-in for Zephyr's sharp,ls0xx display driver (drivers/display/ls0xx.c),
 * bound to the overlay's chosen zephyr,display node.  It reproduces the real
 * driver's API contract (capabilities, MONO01 format, full-width-line writes,
 * x must be 0, blanking unsupported without disp-en-gpios) and replaces the
 * SPI traffic with a framebuffer the host can show.
 *
 * Optional bus timing (PANEL_EMU=memlcd): each write blocks the display work
 * queue for the SPI transfer time at the overlay's spi-max-frequency.
 *
 * VCOM: the overlay has neither extcomin-gpios nor (Zephyr 4.1)
 * serial-vcom-inversion, so the real driver starts no VCOM toggle thread, and
 * its write command byte (0x01) never sets the serial VCOM bit (0x02) either.
 * Nothing to model here; the host reports this as "VCOM static".
 *
 * Pixel data: Zephyr 4.1's lvgl_display_mono.c converts LVGL 9's I1 draw
 * buffer (8-byte palette header + MSB-first bits) into the driver format
 * (LSB-first unless SCREEN_INFO_MONO_MSB_FIRST, MONO01: 1 = white) before
 * calling display_write(), so this driver sees exactly what ls0xx.c sees.
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include "sim_internal.h"

#define DISP_NODE DT_CHOSEN(zephyr_display)
#define PANEL_W DT_PROP(DISP_NODE, width)
#define PANEL_H DT_PROP(DISP_NODE, height)
#define PANEL_SPI_HZ DT_PROP(DISP_NODE, spi_max_frequency)

static uint8_t fb[PANEL_W * PANEL_H]; /* 1 = white (MONO01 bit set) */
uint32_t sim_display_writes;

static int d_blanking_off(const struct device *dev) {
#if DT_NODE_HAS_PROP(DISP_NODE, disp_en_gpios)
    return 0;
#else
    LOG_WRN("Unsupported");
    return -ENOTSUP;
#endif
}

static int d_blanking_on(const struct device *dev) { return d_blanking_off(dev); }

static int d_write(const struct device *dev, const uint16_t x, const uint16_t y,
                   const struct display_buffer_descriptor *desc, const void *buf) {
    if (buf == NULL) {
        LOG_WRN("Display buffer is not available");
        return -EINVAL;
    }
    if (desc->width != PANEL_W) {
        LOG_ERR("Width not a multiple of %d", PANEL_W);
        return -EINVAL;
    }
    if (desc->pitch != desc->width) {
        LOG_ERR("Unsupported mode");
        return -ENOTSUP;
    }
    if ((y + desc->height) > PANEL_H) {
        LOG_ERR("Buffer out of bounds (height)");
        return -EINVAL;
    }
    if (x != 0) {
        LOG_ERR("X-coordinate has to be 0");
        return -EINVAL;
    }
    /* ls0xx shifts each line out as PANEL_W/8 bytes; Zephyr's mono LVGL glue
     * packs pixels LSB-first when SCREEN_INFO_MONO_MSB_FIRST is not set */
    const uint8_t *src = buf;
    for (int row = 0; row < desc->height; row++) {
        for (int col = 0; col < PANEL_W; col++) {
            fb[(y + row) * PANEL_W + col] = (src[col / 8] >> (col % 8)) & 1;
        }
        src += PANEL_W / 8;
    }
    sim_display_writes++;

    /* ls0xx_update_display(): spi_write_dt() is synchronous, so the calling
     * (display) thread is busy for the whole transfer.  Bytes on the bus:
     * write command (1) + per line {address (1), data (W/8), dummy (1)} +
     * trailing byte (1).  Chip-select setup/hold times are ignored. */
    int64_t start_ticks = k_uptime_ticks();
    int xfer_us = 0;
    if (sim_display_spi_timing) {
        int64_t bits = 8LL * (2 + (int64_t)desc->height * (PANEL_W / 8 + 2));
        xfer_us = (int)((bits * 1000000LL + PANEL_SPI_HZ - 1) / PANEL_SPI_HZ);
        /* two flushes inside one LVGL refresh run back to back */
        start_ticks = MAX(start_ticks, sim_block_current_queue_get());
        sim_block_current_queue_until(start_ticks +
                                      (int64_t)k_us_to_ticks_ceil64((uint64_t)xfer_us));
    }
    if (sim_host && sim_host->display_write) {
        sim_host->display_write(sim_host->ctx, -1, (int64_t)k_ticks_to_us_floor64(start_ticks), y,
                                y + desc->height - 1, fb, PANEL_W, PANEL_H, xfer_us);
    }
    return 0;
}

static int d_read(const struct device *dev, const uint16_t x, const uint16_t y,
                  const struct display_buffer_descriptor *desc, void *buf) {
    LOG_ERR("not supported");
    return -ENOTSUP;
}
static void *d_get_framebuffer(const struct device *dev) {
    LOG_ERR("not supported");
    return NULL;
}
static int d_set_brightness(const struct device *dev, const uint8_t b) {
    LOG_WRN("not supported");
    return -ENOTSUP;
}
static int d_set_contrast(const struct device *dev, uint8_t c) {
    LOG_WRN("not supported");
    return -ENOTSUP;
}
static void d_get_capabilities(const struct device *dev, struct display_capabilities *caps) {
    memset(caps, 0, sizeof(struct display_capabilities));
    caps->x_resolution = PANEL_W;
    caps->y_resolution = PANEL_H;
    caps->supported_pixel_formats = PIXEL_FORMAT_MONO01;
    caps->current_pixel_format = PIXEL_FORMAT_MONO01;
    caps->screen_info = SCREEN_INFO_X_ALIGNMENT_WIDTH;
}
static int d_set_orientation(const struct device *dev, const enum display_orientation o) {
    LOG_ERR("Unsupported");
    return -ENOTSUP;
}
static int d_set_pixel_format(const struct device *dev, const enum display_pixel_format pf) {
    if (pf == PIXEL_FORMAT_MONO01) {
        return 0;
    }
    LOG_ERR("not supported");
    return -ENOTSUP;
}

static const struct display_driver_api sim_ls0xx_api = {
    .blanking_on = d_blanking_on,
    .blanking_off = d_blanking_off,
    .write = d_write,
    .read = d_read,
    .get_framebuffer = d_get_framebuffer,
    .set_brightness = d_set_brightness,
    .set_contrast = d_set_contrast,
    .get_capabilities = d_get_capabilities,
    .set_pixel_format = d_set_pixel_format,
    .set_orientation = d_set_orientation,
};

static int sim_ls0xx_init(const struct device *dev) {
    /* the real driver ends init with the panel's "all clear" command, which
     * turns every pixel white (MONO01 bit = 1) */
    memset(fb, 1, sizeof(fb));
    return 0;
}

/* ls0xx registers at POST_KERNEL with CONFIG_DISPLAY_INIT_PRIORITY (default 85) */
DEVICE_DT_DEFINE(DISP_NODE, sim_ls0xx_init, NULL, NULL, NULL, POST_KERNEL, 85, &sim_ls0xx_api);
