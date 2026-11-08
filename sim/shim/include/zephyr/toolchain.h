/*
 * Simulator wrapper around Zephyr's real <zephyr/toolchain.h>.
 *
 * Zephyr's gcc.h refuses to build for an unknown architecture.  We present the
 * POSIX arch (the host-native Zephyr port) ONLY while the toolchain header is
 * parsed, then remove the symbol again so firmware code still sees the real
 * nRF52840 configuration (CONFIG_ARCH_POSIX unset).
 */
#ifndef SIM_ZEPHYR_TOOLCHAIN_WRAPPER_H
#define SIM_ZEPHYR_TOOLCHAIN_WRAPPER_H

#ifndef CONFIG_ARCH_POSIX
#define CONFIG_ARCH_POSIX 1
#define SIM_TEMP_ARCH_POSIX 1
#endif

#include_next <zephyr/toolchain.h>

#ifdef SIM_TEMP_ARCH_POSIX
#undef CONFIG_ARCH_POSIX
#undef SIM_TEMP_ARCH_POSIX
#endif

/* System calls are plain inline calls to z_impl_* (no userspace). */
#undef __syscall
#define __syscall static inline
#ifndef __subsystem
#define __subsystem
#endif

#endif
