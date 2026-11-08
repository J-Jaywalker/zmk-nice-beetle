/*
 * sim: replacement for <zephyr/tracing/tracing.h> (CONFIG_TRACING=n).  The real
 * header pulls in tracking.h -> kernel_structs.h (per-CPU scheduler structs),
 * which the simulator's kernel model does not have; with tracing disabled all
 * it contributes are the no-op SYS_PORT_TRACING_* macros from tracing_macros.h.
 */
#ifndef ZEPHYR_INCLUDE_TRACING_TRACING_H_
#define ZEPHYR_INCLUDE_TRACING_TRACING_H_
#include <zephyr/kernel.h>
#include <zephyr/tracing/tracing_macros.h>
#define sys_trace_sys_init_enter(entry, level)
#define sys_trace_sys_init_exit(entry, level, result)
#endif
