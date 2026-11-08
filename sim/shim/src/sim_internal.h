/* Internal glue shared by the simulator shim sources inside one MCU DLL. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../../host/sim_api.h"

enum { SIM_LOG_ERR = 1, SIM_LOG_WRN = 2, SIM_LOG_INF = 3, SIM_LOG_DBG = 4 };

void sim_log(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
extern const struct sim_host_callbacks *sim_host;
extern int sim_log_level;

int64_t sim_kernel_next_event(void);
void sim_kernel_run_until(int64_t t_ms);
void sim_kernel_run_pending(void);
void sim_run_sys_init(void);
void sim_block_current_queue_until(int64_t until);
void sim_split_rx(const void *data, size_t len);
void sim_fake_set_usb_boot(int s);
extern bool sim_display_spi_timing;
int64_t sim_block_current_queue_get(void);

/* state owned by the fake drivers (fake_hw.c) */
struct sim_hw_state {
    uint8_t battery_level;
    bool usb_powered;
    bool usb_hid_ready;
    bool split_connected;
};
extern struct sim_hw_state sim_hw;
