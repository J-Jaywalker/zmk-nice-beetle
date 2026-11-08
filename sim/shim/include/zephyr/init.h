/* Simulator replacement for <zephyr/init.h>: SYS_INIT entries are collected in
 * an iterable section and run by sim_run_sys_init() sorted by (level, priority,
 * link order) - the same ordering rule Zephyr's linker script implements. */
#ifndef ZEPHYR_INCLUDE_INIT_H_
#define ZEPHYR_INCLUDE_INIT_H_
#include <zephyr/toolchain.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/iterable_sections.h>

struct device;

enum {
    SIM_INIT_EARLY = 0,
    SIM_INIT_PRE_KERNEL_1,
    SIM_INIT_PRE_KERNEL_2,
    SIM_INIT_POST_KERNEL,
    SIM_INIT_APPLICATION,
    SIM_INIT_SMP,
};
#define SIM_INIT_LEVEL(_lvl) _CONCAT(SIM_INIT_, _lvl)

#ifndef SIM_LINK_ORDER
#define SIM_LINK_ORDER 9999
#endif

struct sim_init_entry {
    int level;
    int prio;
    int order;
    int (*sys_init)(void);
    int (*dev_init)(const struct device *dev);
    const struct device *dev;
    const char *name;
};

#define SYS_INIT_NAMED(_nm, _init_fn, _lvl, _prio)                                                 \
    static const TYPE_SECTION_ITERABLE(struct sim_init_entry, _CONCAT(__init_sys_, _nm),           \
                                       sim_init_entry, _nm) = {                                    \
        .level = SIM_INIT_LEVEL(_lvl),                                                             \
        .prio = (_prio),                                                                           \
        .order = SIM_LINK_ORDER,                                                                   \
        .sys_init = (_init_fn),                                                                    \
        .name = #_init_fn,                                                                         \
    }
#define SYS_INIT(_init_fn, _lvl, _prio) SYS_INIT_NAMED(_init_fn, _init_fn, _lvl, _prio)
#endif
