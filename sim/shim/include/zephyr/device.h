/*
 * Simulator replacement for <zephyr/device.h>: the same object model
 * (struct device, DEVICE_DT_DEFINE, DEVICE_DT_GET, device_is_ready, init
 * levels/priorities) without Zephyr's linker-script machinery.
 */
#ifndef ZEPHYR_INCLUDE_DEVICE_H_
#define ZEPHYR_INCLUDE_DEVICE_H_

#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/toolchain.h>
#include <stdbool.h>
#include <stdint.h>

struct device_state {
    int init_res;
    bool initialized;
};

struct pm_device;

struct device {
    const char *name;
    const void *config;
    const void *api;
    struct device_state *state;
    void *data;
};

typedef int16_t device_handle_t;

#define DEVICE_HANDLE_NULL 0

#define DEVICE_NAME_GET(dev_id) _CONCAT(__device_, dev_id)
#define DEVICE_DT_NAME(node_id) DT_PROP_OR(node_id, label, DT_NODE_FULL_NAME(node_id))
#define Z_DEVICE_DT_DEV_ID(node_id) _CONCAT(dts_ord_, DT_DEP_ORD(node_id))
#define DEVICE_DT_NAME_GET(node_id) DEVICE_NAME_GET(Z_DEVICE_DT_DEV_ID(node_id))
#define DEVICE_DT_GET(node_id) (&DEVICE_DT_NAME_GET(node_id))
#define DEVICE_DT_INST_GET(inst) DEVICE_DT_GET(DT_DRV_INST(inst))
#define DEVICE_DT_GET_ANY(compat)                                                                  \
    COND_CODE_1(DT_HAS_COMPAT_STATUS_OKAY(compat),                                                 \
                (DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(compat))), (NULL))
#define DEVICE_DT_GET_ONE(compat) DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(compat))
#define DEVICE_DT_GET_OR_NULL(node_id)                                                             \
    COND_CODE_1(DT_NODE_HAS_STATUS(node_id, okay), (DEVICE_DT_GET(node_id)), (NULL))
#define DEVICE_GET(dev_id) (&DEVICE_NAME_GET(dev_id))
#define DEVICE_DECLARE(dev_id) extern struct device DEVICE_NAME_GET(dev_id)
#define DEVICE_DT_DECLARE(node_id) extern struct device DEVICE_DT_NAME_GET(node_id)

#define Z_DEVICE_STATE_NAME(dev_id) _CONCAT(__devstate_, dev_id)

#define Z_SIM_DEVICE_DEFINE(node_id, dev_id, dev_name, _init_fn, data_ptr, cfg_ptr, _lvl, _prio,  \
                            api_ptr)                                                               \
    static struct device_state Z_DEVICE_STATE_NAME(dev_id);                                        \
    TYPE_SECTION_ITERABLE(struct device, DEVICE_NAME_GET(dev_id), device, dev_id) = {              \
        .name = dev_name,                                                                          \
        .config = (cfg_ptr),                                                                       \
        .api = (api_ptr),                                                                          \
        .state = &Z_DEVICE_STATE_NAME(dev_id),                                                     \
        .data = (data_ptr),                                                                        \
    };                                                                                             \
    static const TYPE_SECTION_ITERABLE(struct sim_init_entry, _CONCAT(__init_, dev_id),            \
                                       sim_init_entry, dev_id) = {                                 \
        .level = SIM_INIT_LEVEL(_lvl),                                                             \
        .prio = (_prio),                                                                           \
        .order = SIM_LINK_ORDER,                                                                   \
        .dev_init = (_init_fn),                                                                    \
        .dev = &DEVICE_NAME_GET(dev_id),                                                           \
        .name = dev_name,                                                                          \
    }

#define DEVICE_DT_DEFINE(node_id, _init_fn, _pm, _data, _cfg, _lvl, _prio, _api, ...)             \
    Z_SIM_DEVICE_DEFINE(node_id, Z_DEVICE_DT_DEV_ID(node_id), DEVICE_DT_NAME(node_id), _init_fn,   \
                        _data, _cfg, _lvl, _prio, _api)
#define DEVICE_DT_INST_DEFINE(inst, ...) DEVICE_DT_DEFINE(DT_DRV_INST(inst), __VA_ARGS__)
#define DEVICE_DEFINE(dev_id, _nm, _init_fn, _pm, _data, _cfg, _lvl, _prio, _api)                 \
    Z_SIM_DEVICE_DEFINE(DT_INVALID_NODE, dev_id, _nm, _init_fn, _data, _cfg, _lvl, _prio, _api)

static inline bool device_is_ready(const struct device *dev) {
    return dev != NULL && dev->state->initialized && dev->state->init_res == 0;
}
static inline bool z_device_is_ready(const struct device *dev) { return device_is_ready(dev); }
const struct device *device_get_binding(const char *name);
int device_init(const struct device *dev);

/* extern declarations for every enabled devicetree node, like Zephyr does */
#define Z_MAYBE_DEVICE_DECLARE_INTERNAL(node_id) extern struct device DEVICE_DT_NAME_GET(node_id);
DT_FOREACH_STATUS_OKAY_NODE(Z_MAYBE_DEVICE_DECLARE_INTERNAL)

#endif
