/*
 * Simulator replacement for <zephyr/kernel.h>.
 *
 * Provides a deterministic, single-threaded, virtual-time model of the Zephyr
 * kernel objects that ZMK and the widgets use: work queues (with Zephyr's
 * submit/schedule/cancel semantics and per-queue priority), delayable work,
 * timers (expiry callbacks run "in ISR context" at their deadline), mutexes,
 * semaphores, message queues and uptime.  Each simulated MCU (central and
 * peripheral DLL) has its own copy of this state and its own clock.
 *
 * Threads are not simulated: work items run to completion instantly in
 * virtual time, in priority order (lower number = higher priority, as in
 * Zephyr).  See sim/README.md for the consequences.
 */
#ifndef ZEPHYR_INCLUDE_KERNEL_H_
#define ZEPHYR_INCLUDE_KERNEL_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include <zephyr/toolchain.h>
#include <zephyr/types.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/slist.h>
#include <zephyr/sys/dlist.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/__assert.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/arch/common/sys_bitops.h> /* sys_bitfield_*() as on Cortex-M */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- time */

/* Ticks run at CONFIG_SYS_CLOCK_TICKS_PER_SEC (32768 Hz on nRF52, like the
 * real RTC-driven kernel clock); ms<->tick conversions use Zephyr's own
 * time_units.h macros, so rounding matches the firmware. */
#include <zephyr/sys/time_units.h>
typedef int64_t k_ticks_t;
typedef struct {
    k_ticks_t ticks;
} k_timeout_t;

#define SIM_TIMEOUT_FOREVER ((k_ticks_t)-1)
#define K_NO_WAIT ((k_timeout_t){0})
#define K_FOREVER ((k_timeout_t){SIM_TIMEOUT_FOREVER})
#define K_TICKS(t) ((k_timeout_t){(k_ticks_t)(t)})
#define K_MSEC(ms) K_TICKS(k_ms_to_ticks_ceil64(MAX((int64_t)(ms), 0)))
#define K_USEC(us) K_TICKS(k_us_to_ticks_ceil64(MAX((int64_t)(us), 0)))
#define K_NSEC(ns) K_TICKS(k_ns_to_ticks_ceil64(MAX((int64_t)(ns), 0)))
#define K_SECONDS(s) K_MSEC((s) * 1000)
#define K_MINUTES(m) K_SECONDS((m) * 60)
#define K_HOURS(h) K_MINUTES((h) * 60)
#define K_TIMEOUT_EQ(a, b) ((a).ticks == (b).ticks)

int64_t k_uptime_ticks(void);
static inline int64_t k_uptime_get(void) { return k_ticks_to_ms_floor64(k_uptime_ticks()); }
static inline uint32_t k_uptime_get_32(void) { return (uint32_t)k_uptime_get(); }
static inline int64_t k_uptime_delta(int64_t *reftime) {
    int64_t now = k_uptime_get(), d = now - *reftime;
    *reftime = now;
    return d;
}
static inline uint32_t k_cycle_get_32(void) { return (uint32_t)k_uptime_ticks(); }
static inline uint64_t k_cycle_get_64(void) { return (uint64_t)k_uptime_ticks(); }

/* -------------------------------------------------------------- objects */

struct k_work;
struct k_work_q;
typedef void (*k_work_handler_t)(struct k_work *work);

#define SIM_WORK_QUEUED BIT(0)
#define SIM_WORK_RUNNING BIT(1)
#define SIM_WORK_DELAYED BIT(2)
#define SIM_WORK_CANCELING BIT(3)

struct k_work {
    struct k_work *sim_next;
    k_work_handler_t handler;
    struct k_work_q *queue;
    uint32_t flags;
};

struct k_work_q {
    const char *sim_name;
    int sim_prio;
    bool sim_started;
    struct k_work *sim_head, *sim_tail;
    struct k_work_q *sim_next_q;
    int64_t sim_blocked_until; /* used to model a blocking display flush */
};

struct k_work_queue_config {
    const char *name;
    bool no_yield;
    bool essential;
};

struct sim_timeout {
    struct sim_timeout *sim_next;
    int64_t deadline;
    bool active;
    void (*fn)(struct sim_timeout *t);
};

struct k_work_delayable {
    struct k_work work;
    struct sim_timeout timeout;
    struct k_work_q *queue;
};

struct k_work_sync {
    int dummy;
};

struct k_timer;
typedef void (*k_timer_expiry_t)(struct k_timer *timer);
typedef void (*k_timer_stop_t)(struct k_timer *timer);

struct k_timer {
    struct sim_timeout timeout;
    k_timer_expiry_t expiry_fn;
    k_timer_stop_t stop_fn;
    k_ticks_t period;
    uint32_t status;
    void *user_data;
};

struct k_mutex {
    int lock_count;
};
struct k_sem {
    unsigned int count, limit;
};
struct k_msgq {
    char *buffer_start;
    size_t msg_size;
    uint32_t max_msgs;
    uint32_t used_msgs;
    uint32_t read_idx, write_idx;
};
struct k_spinlock {
    int dummy;
};
typedef int k_spinlock_key_t;
struct k_thread {
    int dummy;
};
typedef struct k_thread *k_tid_t;
typedef char k_thread_stack_t;

/* ---------------------------------------------------------- work items */

#define Z_WORK_INITIALIZER(work_handler) {.handler = (work_handler)}
#define K_WORK_DEFINE(work, work_handler) struct k_work work = Z_WORK_INITIALIZER(work_handler)
#define Z_WORK_DELAYABLE_INITIALIZER(work_handler)                                                 \
    {.work = {.handler = (work_handler), .flags = 0}, .timeout = {.fn = sim_dwork_timeout_fn}}
#define K_WORK_DELAYABLE_DEFINE(work, work_handler)                                                \
    struct k_work_delayable work = Z_WORK_DELAYABLE_INITIALIZER(work_handler)

void sim_dwork_timeout_fn(struct sim_timeout *t);

extern struct k_work_q k_sys_work_q;

void k_work_init(struct k_work *work, k_work_handler_t handler);
int k_work_submit_to_queue(struct k_work_q *queue, struct k_work *work);
static inline int k_work_submit(struct k_work *work) {
    return k_work_submit_to_queue(&k_sys_work_q, work);
}
int k_work_busy_get(const struct k_work *work);
static inline bool k_work_is_pending(const struct k_work *work) { return k_work_busy_get(work) != 0; }
int k_work_cancel(struct k_work *work);
bool k_work_cancel_sync(struct k_work *work, struct k_work_sync *sync);
bool k_work_flush(struct k_work *work, struct k_work_sync *sync);

void k_work_init_delayable(struct k_work_delayable *dwork, k_work_handler_t handler);
static inline struct k_work_delayable *k_work_delayable_from_work(struct k_work *work) {
    return CONTAINER_OF(work, struct k_work_delayable, work);
}
int k_work_schedule_for_queue(struct k_work_q *queue, struct k_work_delayable *dwork,
                              k_timeout_t delay);
static inline int k_work_schedule(struct k_work_delayable *dwork, k_timeout_t delay) {
    return k_work_schedule_for_queue(&k_sys_work_q, dwork, delay);
}
int k_work_reschedule_for_queue(struct k_work_q *queue, struct k_work_delayable *dwork,
                                k_timeout_t delay);
static inline int k_work_reschedule(struct k_work_delayable *dwork, k_timeout_t delay) {
    return k_work_reschedule_for_queue(&k_sys_work_q, dwork, delay);
}
int k_work_cancel_delayable(struct k_work_delayable *dwork);
bool k_work_cancel_delayable_sync(struct k_work_delayable *dwork, struct k_work_sync *sync);
int k_work_delayable_busy_get(const struct k_work_delayable *dwork);
static inline bool k_work_delayable_is_pending(const struct k_work_delayable *dwork) {
    return k_work_delayable_busy_get(dwork) != 0;
}
k_ticks_t k_work_delayable_remaining_get(const struct k_work_delayable *dwork);
static inline k_ticks_t k_work_delayable_expires_get(const struct k_work_delayable *dwork) {
    return dwork->timeout.active ? dwork->timeout.deadline : 0;
}

void k_work_queue_init(struct k_work_q *queue);
void k_work_queue_start(struct k_work_q *queue, k_thread_stack_t *stack, size_t stack_size,
                        int prio, const struct k_work_queue_config *cfg);

/* --------------------------------------------------------------- timers */

void sim_timer_timeout_fn(struct sim_timeout *t);
#define Z_TIMER_INITIALIZER(obj, expiry, stop)                                                     \
    {.timeout = {.fn = sim_timer_timeout_fn}, .expiry_fn = (expiry), .stop_fn = (stop)}
#define K_TIMER_DEFINE(name, expiry_fn, stop_fn)                                                   \
    struct k_timer name = Z_TIMER_INITIALIZER(name, expiry_fn, stop_fn)

void k_timer_init(struct k_timer *timer, k_timer_expiry_t expiry_fn, k_timer_stop_t stop_fn);
void k_timer_start(struct k_timer *timer, k_timeout_t duration, k_timeout_t period);
void k_timer_stop(struct k_timer *timer);
uint32_t k_timer_status_get(struct k_timer *timer);
k_ticks_t k_timer_remaining_ticks(const struct k_timer *timer);
static inline uint32_t k_timer_remaining_get(struct k_timer *timer) {
    return k_ticks_to_ms_floor32(k_timer_remaining_ticks(timer));
}
static inline void k_timer_user_data_set(struct k_timer *timer, void *user_data) {
    timer->user_data = user_data;
}
static inline void *k_timer_user_data_get(const struct k_timer *timer) { return timer->user_data; }

/* ------------------------------------------------- mutex / sem / msgq */

#define K_MUTEX_DEFINE(name) struct k_mutex name = {0}
static inline int k_mutex_init(struct k_mutex *m) {
    m->lock_count = 0;
    return 0;
}
static inline int k_mutex_lock(struct k_mutex *m, k_timeout_t t) {
    ARG_UNUSED(t);
    m->lock_count++;
    return 0;
}
static inline int k_mutex_unlock(struct k_mutex *m) {
    if (m->lock_count > 0) {
        m->lock_count--;
    }
    return 0;
}

#define K_SEM_DEFINE(name, initial_count, count_limit)                                             \
    struct k_sem name = {.count = (initial_count), .limit = (count_limit)}
#define K_SEM_MAX_LIMIT UINT32_MAX
static inline int k_sem_init(struct k_sem *s, unsigned int initial, unsigned int limit) {
    s->count = initial;
    s->limit = limit;
    return 0;
}
int k_sem_take(struct k_sem *sem, k_timeout_t timeout);
static inline void k_sem_give(struct k_sem *s) {
    if (s->count < s->limit) {
        s->count++;
    }
}
static inline void k_sem_reset(struct k_sem *s) { s->count = 0; }
static inline unsigned int k_sem_count_get(struct k_sem *s) { return s->count; }

#define K_MSGQ_DEFINE(q_name, q_msg_size, q_max_msgs, q_align)                                    \
    static char __aligned(q_align) _k_fifo_buf_##q_name[(q_max_msgs) * (q_msg_size)];              \
    struct k_msgq q_name = {.buffer_start = _k_fifo_buf_##q_name,                                 \
                            .msg_size = (q_msg_size),                                              \
                            .max_msgs = (q_max_msgs)}
void k_msgq_init(struct k_msgq *q, char *buffer, size_t msg_size, uint32_t max_msgs);
int k_msgq_put(struct k_msgq *q, const void *data, k_timeout_t timeout);
int k_msgq_get(struct k_msgq *q, void *data, k_timeout_t timeout);
int k_msgq_peek(struct k_msgq *q, void *data);
void k_msgq_purge(struct k_msgq *q);
static inline uint32_t k_msgq_num_used_get(struct k_msgq *q) { return q->used_msgs; }
static inline uint32_t k_msgq_num_free_get(struct k_msgq *q) { return q->max_msgs - q->used_msgs; }

static inline k_spinlock_key_t k_spin_lock(struct k_spinlock *l) {
    ARG_UNUSED(l);
    return 0;
}
static inline void k_spin_unlock(struct k_spinlock *l, k_spinlock_key_t key) {
    ARG_UNUSED(l);
    ARG_UNUSED(key);
}
static inline unsigned int irq_lock(void) { return 0; }
static inline void irq_unlock(unsigned int key) { ARG_UNUSED(key); }

/* -------------------------------------------------------------- threads */

#define K_KERNEL_STACK_DEFINE(sym, size) char sym[size]
#define K_THREAD_STACK_DEFINE(sym, size) char sym[size]
#define K_THREAD_STACK_SIZEOF(sym) sizeof(sym)
#define K_PRIO_COOP(x) (-((x) + 1))
#define K_PRIO_PREEMPT(x) (x)
#define K_ESSENTIAL 0
/* No simulated thread may be defined statically; fail loudly if firmware tries. */
#define K_THREAD_DEFINE(name, ...)                                                                 \
    BUILD_ASSERT(0, "K_THREAD_DEFINE is not supported by the simulator kernel")

bool k_is_in_isr(void);
int32_t k_sleep(k_timeout_t timeout);
static inline int32_t k_msleep(int32_t ms) { return k_sleep(K_MSEC(ms)); }
static inline int32_t k_usleep(int32_t us) { return k_sleep(K_USEC(us)); }
static inline void k_busy_wait(uint32_t us) { ARG_UNUSED(us); }
static inline void k_yield(void) {}
static inline k_tid_t k_current_get(void) { return NULL; }
static inline void *k_malloc(size_t size) { return malloc(size); }
static inline void *k_calloc(size_t n, size_t size) { return calloc(n, size); }
static inline void k_free(void *ptr) { free(ptr); }

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_KERNEL_H_ */
