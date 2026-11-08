/*
 * Simulator stand-in for <zephyr/rtio/rtio.h>.  Only the declarations that
 * <zephyr/drivers/sensor.h> needs to compile; the asynchronous RTIO sensor API
 * is not used by ZMK or the widgets.
 */
#ifndef ZEPHYR_INCLUDE_RTIO_RTIO_H_
#define ZEPHYR_INCLUDE_RTIO_RTIO_H_
#include <zephyr/kernel.h>

#define RTIO_PRIO_LOW 0U
#define RTIO_PRIO_NORM 127U
#define RTIO_PRIO_HIGH 255U

struct rtio;
struct rtio_cqe;
struct rtio_iodev_sqe;
struct rtio_iodev_api {
    void (*submit)(struct rtio_iodev_sqe *iodev_sqe);
};
struct rtio_iodev {
    const struct rtio_iodev_api *api;
    void *data;
};
struct rtio_sqe {
    uint8_t op;
    uint8_t prio;
    uint16_t flags;
    const struct rtio_iodev *iodev;
    void *userdata;
};

#define RTIO_IODEV_DEFINE(name, iodev_api, iodev_data)                                             \
    struct rtio_iodev name = {.api = (iodev_api), .data = (iodev_data)}

void rtio_sqe_prep_read_with_pool(struct rtio_sqe *sqe, const struct rtio_iodev *iodev,
                                  int8_t prio, void *userdata);
int rtio_sqe_copy_in(struct rtio *r, const struct rtio_sqe *sqes, size_t sqe_count);
struct rtio_sqe *rtio_sqe_acquire(struct rtio *r);
int rtio_submit(struct rtio *r, uint32_t wait_count);
#endif
