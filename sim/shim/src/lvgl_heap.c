/*
 * Observes the LVGL memory pool.  Zephyr's real lvgl_mem.c and the real
 * sys_heap allocator (lib/os/heap.c) are compiled unchanged; the link step
 * wraps sys_heap_init/alloc/realloc (-Wl,--wrap=...) so we can find the pool
 * and report exhaustion, which on hardware would crash or hang LVGL.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_heap.h>
#include <zephyr/sys/mem_stats.h>
#include "sim_internal.h"

static struct sys_heap *lvgl_pool;
static size_t pool_bytes;

void __real_sys_heap_init(struct sys_heap *heap, void *mem, size_t bytes);
void *__real_sys_heap_alloc(struct sys_heap *heap, size_t bytes);
void *__real_sys_heap_aligned_realloc(struct sys_heap *heap, void *ptr, size_t align, size_t bytes);

void __wrap_sys_heap_init(struct sys_heap *heap, void *mem, size_t bytes) {
    lvgl_pool = heap;
    pool_bytes = bytes;
    __real_sys_heap_init(heap, mem, bytes);
}

static void report_oom(size_t bytes) {
    static int reported;
    if (reported++ < 5) {
        sim_log(SIM_LOG_ERR,
                "LVGL heap exhausted: %u byte allocation failed (pool %u bytes, "
                "CONFIG_LV_Z_MEM_POOL_SIZE). On hardware LVGL would crash or hang here.",
                (unsigned)bytes, (unsigned)pool_bytes);
    }
}

void *__wrap_sys_heap_alloc(struct sys_heap *heap, size_t bytes) {
    void *p = __real_sys_heap_alloc(heap, bytes);
    if (!p && bytes) {
        report_oom(bytes);
    }
    return p;
}

void *__wrap_sys_heap_aligned_realloc(struct sys_heap *heap, void *ptr, size_t align, size_t bytes) {
    void *p = __real_sys_heap_aligned_realloc(heap, ptr, align, bytes);
    if (!p && bytes) {
        report_oom(bytes);
    }
    return p;
}

size_t sim_lvgl_heap_used(void) {
    struct sys_memory_stats s;
    if (!lvgl_pool || sys_heap_runtime_stats_get(lvgl_pool, &s)) {
        return 0;
    }
    return s.allocated_bytes;
}

size_t sim_lvgl_heap_peak(void) {
    struct sys_memory_stats s;
    if (!lvgl_pool || sys_heap_runtime_stats_get(lvgl_pool, &s)) {
        return 0;
    }
    return s.max_allocated_bytes;
}
