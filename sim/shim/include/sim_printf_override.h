/*
 * Force-included (-include) into every simulated-firmware translation unit.
 *
 * Zephyr's modules/lvgl/include/lv_conf.h maps lv_snprintf/lv_vsnprintf to
 * snprintf/vsnprintf, and with CONFIG_LV_USE_BUILTIN_SPRINTF (the default)
 * LVGL's lv_sprintf_builtin.c therefore *defines* snprintf()/vsnprintf().  On
 * the nRF52 those definitions win over the C library's at link time, so every
 * snprintf in the firmware (ZMK, widgets, LVGL) runs LVGL's built-in printf.
 *
 * mingw's <stdio.h> defines both as static inlines, which would collide.  So:
 * include <stdio.h> first, then rename the two symbols for the rest of the TU.
 * LVGL's builtin then defines sim_fw_(v)snprintf and all firmware calls go to
 * it - the same implementation (and formatting quirks) as on hardware.
 */
#pragma once
#include <stdio.h>
#include <stdarg.h>
#define snprintf sim_fw_snprintf
#define vsnprintf sim_fw_vsnprintf
int sim_fw_snprintf(char *buffer, size_t count, const char *format, ...);
int sim_fw_vsnprintf(char *buffer, size_t count, const char *format, va_list va);
