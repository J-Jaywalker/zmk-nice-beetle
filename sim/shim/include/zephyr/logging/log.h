/*
 * Simulator logging: LOG_* go to the host log callback, filtered at runtime by
 * SIM_LOG (0=off .. 4=debug).  Compile-time CONFIG_*_LOG_LEVEL is ignored.
 */
#ifndef ZEPHYR_INCLUDE_LOGGING_LOG_H_
#define ZEPHYR_INCLUDE_LOGGING_LOG_H_
#include <zephyr/toolchain.h>
#include <stdint.h>
#include <stddef.h>

void sim_log_module(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void sim_log_hexdump(int level, const void *data, size_t len, const char *msg);

#define LOG_LEVEL_NONE 0
#define LOG_LEVEL_ERR 1
#define LOG_LEVEL_WRN 2
#define LOG_LEVEL_INF 3
#define LOG_LEVEL_DBG 4

#define LOG_MODULE_REGISTER(...)
#define LOG_MODULE_DECLARE(...)
#define LOG_LEVEL_SET(...)

#define SIM_LOG_AT(lvl, fmt, ...) sim_log_module(lvl, "%s: " fmt, __func__, ##__VA_ARGS__)
#define LOG_ERR(...) SIM_LOG_AT(1, __VA_ARGS__)
#define LOG_WRN(...) SIM_LOG_AT(2, __VA_ARGS__)
#define LOG_INF(...) SIM_LOG_AT(3, __VA_ARGS__)
#define LOG_DBG(...) SIM_LOG_AT(4, __VA_ARGS__)
#define LOG_HEXDUMP_ERR(d, l, m) sim_log_hexdump(1, d, l, m)
#define LOG_HEXDUMP_WRN(d, l, m) sim_log_hexdump(2, d, l, m)
#define LOG_HEXDUMP_INF(d, l, m) sim_log_hexdump(3, d, l, m)
#define LOG_HEXDUMP_DBG(d, l, m) sim_log_hexdump(4, d, l, m)
#define LOG_PANIC()
#define LOG_INST_DBG(i, ...) LOG_DBG(__VA_ARGS__)
#define LOG_INST_ERR(i, ...) LOG_ERR(__VA_ARGS__)
#define LOG_INST_WRN(i, ...) LOG_WRN(__VA_ARGS__)
#define LOG_INST_INF(i, ...) LOG_INF(__VA_ARGS__)
#define log_strdup(s) (s)
#define SIM_LOG_ONCE(lvl, ...)                                                                     \
    do {                                                                                           \
        static uint8_t __warned;                                                                   \
        if (__warned == 0) {                                                                       \
            __warned = 1;                                                                          \
            SIM_LOG_AT(lvl, __VA_ARGS__);                                                          \
        }                                                                                          \
    } while (0)
#define LOG_ERR_ONCE(...) SIM_LOG_ONCE(1, __VA_ARGS__)
#define LOG_WRN_ONCE(...) SIM_LOG_ONCE(2, __VA_ARGS__)
#define LOG_INF_ONCE(...) SIM_LOG_ONCE(3, __VA_ARGS__)
#define LOG_DBG_ONCE(...) SIM_LOG_ONCE(4, __VA_ARGS__)
#endif
