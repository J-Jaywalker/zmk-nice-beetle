/*
 * Virtual-time kernel for one simulated MCU (see shim/include/zephyr/kernel.h).
 *
 * Scheduling model
 *   - Time only advances when the host calls sim_kernel_run_until().
 *   - At each instant: expire timeouts (timer expiry fns run as "ISR"),
 *     then run ready work items one at a time, always picking the
 *     highest-priority queue (lowest prio number) that has work and is not
 *     blocked.  A work item runs to completion in zero virtual time.
 *   - A queue can be blocked until a time (used by the display driver to
 *     model the display thread being busy with a synchronous SPI transfer).
 */
#include <zephyr/kernel.h>
#include "sim_internal.h"
#include <setjmp.h>

static jmp_buf reboot_jmp;
static bool reboot_armed;
int sim_reboot_requested = -1;

/* sys_reboot() never returns on hardware.  We unwind to the host-facing
 * run loop and report it; the host then reloads this DLL (a cold boot). */
FUNC_NORETURN void sys_reboot(int type) {
    sim_log(SIM_LOG_INF, "sys_reboot(%d) requested by firmware", type);
    sim_reboot_requested = type;
    if (reboot_armed) {
        longjmp(reboot_jmp, 1);
    }
    for (;;) {
    }
}

static int64_t now_ms; /* NB: in kernel ticks (CONFIG_SYS_CLOCK_TICKS_PER_SEC) */
static bool in_isr;
static struct sim_timeout *timeouts;
static struct k_work_q *queues;

struct k_work_q k_sys_work_q = {.sim_name = "sysworkq", .sim_prio = -1};

int64_t k_uptime_ticks(void) { return now_ms; }
bool k_is_in_isr(void) { return in_isr; }

int32_t k_sleep(k_timeout_t timeout) {
    /* Nothing that the simulator compiles should block; make it visible if it happens. */
    sim_log(SIM_LOG_WRN, "k_sleep(%lld ticks) called; the simulator cannot block, ignoring",
            (long long)timeout.ticks);
    return 0;
}

/* ---------------------------------------------------------- timeouts */

static void timeout_add(struct sim_timeout *t, int64_t deadline) {
    if (!t->active) {
        t->sim_next = timeouts;
        timeouts = t;
    }
    t->active = true;
    t->deadline = deadline;
}

static void timeout_remove(struct sim_timeout *t) {
    if (!t->active) {
        return;
    }
    for (struct sim_timeout **pp = &timeouts; *pp; pp = &(*pp)->sim_next) {
        if (*pp == t) {
            *pp = t->sim_next;
            break;
        }
    }
    t->active = false;
}

/* ------------------------------------------------------------- queues */

static void queue_register(struct k_work_q *q) {
    for (struct k_work_q *it = queues; it; it = it->sim_next_q) {
        if (it == q) {
            return;
        }
    }
    q->sim_next_q = queues;
    queues = q;
}

void k_work_queue_init(struct k_work_q *queue) { ARG_UNUSED(queue); }

void k_work_queue_start(struct k_work_q *queue, k_thread_stack_t *stack, size_t stack_size,
                        int prio, const struct k_work_queue_config *cfg) {
    ARG_UNUSED(stack);
    ARG_UNUSED(stack_size);
    queue->sim_prio = prio;
    queue->sim_started = true;
    if (cfg && cfg->name) {
        queue->sim_name = cfg->name;
    }
    queue_register(queue);
}

static void enqueue(struct k_work_q *q, struct k_work *w) {
    if (!q->sim_started && q != &k_sys_work_q) {
        sim_log(SIM_LOG_WRN, "work submitted to a queue that was never started");
    }
    queue_register(q);
    w->sim_next = NULL;
    if (q->sim_tail) {
        q->sim_tail->sim_next = w;
    } else {
        q->sim_head = w;
    }
    q->sim_tail = w;
    w->queue = q;
    w->flags |= SIM_WORK_QUEUED;
}

static void dequeue(struct k_work *w) {
    struct k_work_q *q = w->queue;
    if (!q || !(w->flags & SIM_WORK_QUEUED)) {
        return;
    }
    struct k_work *prev = NULL;
    for (struct k_work *it = q->sim_head; it; prev = it, it = it->sim_next) {
        if (it == w) {
            if (prev) {
                prev->sim_next = it->sim_next;
            } else {
                q->sim_head = it->sim_next;
            }
            if (q->sim_tail == it) {
                q->sim_tail = prev;
            }
            break;
        }
    }
    w->flags &= ~SIM_WORK_QUEUED;
}

void k_work_init(struct k_work *work, k_work_handler_t handler) {
    memset(work, 0, sizeof(*work));
    work->handler = handler;
}

/* Zephyr semantics: 0 = already queued, 1 = queued, 2 = queued while running */
int k_work_submit_to_queue(struct k_work_q *queue, struct k_work *work) {
    if (work->flags & SIM_WORK_CANCELING) {
        return -EBUSY;
    }
    if (work->flags & SIM_WORK_QUEUED) {
        return 0;
    }
    int ret = 1;
    if (work->flags & SIM_WORK_RUNNING) {
        /* Zephyr resubmits to the queue it's running on */
        queue = work->queue ? work->queue : queue;
        ret = 2;
    }
    enqueue(queue, work);
    return ret;
}

int k_work_busy_get(const struct k_work *work) {
    return work->flags & (SIM_WORK_QUEUED | SIM_WORK_RUNNING | SIM_WORK_DELAYED);
}

int k_work_cancel(struct k_work *work) {
    dequeue(work);
    return k_work_busy_get(work);
}

bool k_work_cancel_sync(struct k_work *work, struct k_work_sync *sync) {
    ARG_UNUSED(sync);
    bool pending = k_work_busy_get(work) != 0;
    dequeue(work);
    return pending;
}

bool k_work_flush(struct k_work *work, struct k_work_sync *sync) {
    ARG_UNUSED(work);
    ARG_UNUSED(sync);
    return false;
}

/* ------------------------------------------------------ delayable work */

void sim_dwork_timeout_fn(struct sim_timeout *t) {
    struct k_work_delayable *dw = CONTAINER_OF(t, struct k_work_delayable, timeout);
    dw->work.flags &= ~SIM_WORK_DELAYED;
    k_work_submit_to_queue(dw->queue ? dw->queue : &k_sys_work_q, &dw->work);
}

void k_work_init_delayable(struct k_work_delayable *dwork, k_work_handler_t handler) {
    memset(dwork, 0, sizeof(*dwork));
    dwork->work.handler = handler;
    dwork->timeout.fn = sim_dwork_timeout_fn;
}

static void schedule(struct k_work_q *queue, struct k_work_delayable *dw, k_timeout_t delay) {
    dw->queue = queue;
    if (delay.ticks == SIM_TIMEOUT_FOREVER) {
        return;
    }
    if (delay.ticks <= 0) {
        k_work_submit_to_queue(queue, &dw->work);
        return;
    }
    dw->work.flags |= SIM_WORK_DELAYED;
    dw->timeout.fn = sim_dwork_timeout_fn;
    timeout_add(&dw->timeout, now_ms + delay.ticks);
}

/* 0 = already scheduled/queued (no change), 1 = scheduled */
int k_work_schedule_for_queue(struct k_work_q *queue, struct k_work_delayable *dwork,
                              k_timeout_t delay) {
    if (dwork->work.flags & (SIM_WORK_QUEUED | SIM_WORK_DELAYED)) {
        return 0;
    }
    schedule(queue, dwork, delay);
    return 1;
}

int k_work_reschedule_for_queue(struct k_work_q *queue, struct k_work_delayable *dwork,
                                k_timeout_t delay) {
    timeout_remove(&dwork->timeout);
    dwork->work.flags &= ~SIM_WORK_DELAYED;
    if (dwork->work.flags & SIM_WORK_QUEUED) {
        /* Zephyr: if already queued and delay is non-zero, it's also (re)scheduled; keep simple */
        if (delay.ticks <= 0) {
            return 0;
        }
        dequeue(&dwork->work);
    }
    schedule(queue, dwork, delay);
    return 1;
}

int k_work_cancel_delayable(struct k_work_delayable *dwork) {
    timeout_remove(&dwork->timeout);
    dwork->work.flags &= ~SIM_WORK_DELAYED;
    dequeue(&dwork->work);
    return k_work_busy_get(&dwork->work);
}

bool k_work_cancel_delayable_sync(struct k_work_delayable *dwork, struct k_work_sync *sync) {
    ARG_UNUSED(sync);
    bool pending = k_work_delayable_busy_get(dwork) != 0;
    k_work_cancel_delayable(dwork);
    return pending;
}

int k_work_delayable_busy_get(const struct k_work_delayable *dwork) {
    return k_work_busy_get(&dwork->work);
}

k_ticks_t k_work_delayable_remaining_get(const struct k_work_delayable *dwork) {
    return dwork->timeout.active ? MAX(0, dwork->timeout.deadline - now_ms) : 0;
}

/* --------------------------------------------------------------- timers */

void sim_timer_timeout_fn(struct sim_timeout *t) {
    struct k_timer *timer = CONTAINER_OF(t, struct k_timer, timeout);
    timer->status++;
    if (timer->period > 0) {
        timeout_add(&timer->timeout, timer->timeout.deadline + timer->period);
    }
    if (timer->expiry_fn) {
        timer->expiry_fn(timer);
    }
}

void k_timer_init(struct k_timer *timer, k_timer_expiry_t expiry_fn, k_timer_stop_t stop_fn) {
    memset(timer, 0, sizeof(*timer));
    timer->expiry_fn = expiry_fn;
    timer->stop_fn = stop_fn;
    timer->timeout.fn = sim_timer_timeout_fn;
}

void k_timer_start(struct k_timer *timer, k_timeout_t duration, k_timeout_t period) {
    if (duration.ticks == SIM_TIMEOUT_FOREVER) {
        return;
    }
    timeout_remove(&timer->timeout);
    timer->timeout.fn = sim_timer_timeout_fn;
    timer->period = period.ticks == SIM_TIMEOUT_FOREVER ? 0 : period.ticks;
    timer->status = 0;
    /* Zephyr: a zero duration still expires at the next tick */
    timeout_add(&timer->timeout, now_ms + MAX(duration.ticks, 0));
}

void k_timer_stop(struct k_timer *timer) {
    bool was_active = timer->timeout.active;
    timeout_remove(&timer->timeout);
    if (was_active && timer->stop_fn) {
        timer->stop_fn(timer);
    }
}

uint32_t k_timer_status_get(struct k_timer *timer) {
    uint32_t s = timer->status;
    timer->status = 0;
    return s;
}

k_ticks_t k_timer_remaining_ticks(const struct k_timer *timer) {
    return timer->timeout.active ? MAX(0, timer->timeout.deadline - now_ms) : 0;
}

/* --------------------------------------------------------- sem / msgq */

int k_sem_take(struct k_sem *sem, k_timeout_t timeout) {
    if (sem->count > 0) {
        sem->count--;
        return 0;
    }
    if (timeout.ticks != 0) {
        sim_log(SIM_LOG_WRN, "k_sem_take would block; the simulator cannot block");
    }
    return timeout.ticks == 0 ? -EBUSY : -EAGAIN;
}

void k_msgq_init(struct k_msgq *q, char *buffer, size_t msg_size, uint32_t max_msgs) {
    memset(q, 0, sizeof(*q));
    q->buffer_start = buffer;
    q->msg_size = msg_size;
    q->max_msgs = max_msgs;
}

int k_msgq_put(struct k_msgq *q, const void *data, k_timeout_t timeout) {
    ARG_UNUSED(timeout);
    if (q->used_msgs >= q->max_msgs) {
        return timeout.ticks == 0 ? -ENOMSG : -EAGAIN;
    }
    memcpy(q->buffer_start + q->write_idx * q->msg_size, data, q->msg_size);
    q->write_idx = (q->write_idx + 1) % q->max_msgs;
    q->used_msgs++;
    return 0;
}

int k_msgq_get(struct k_msgq *q, void *data, k_timeout_t timeout) {
    if (q->used_msgs == 0) {
        return timeout.ticks == 0 ? -ENOMSG : -EAGAIN;
    }
    memcpy(data, q->buffer_start + q->read_idx * q->msg_size, q->msg_size);
    q->read_idx = (q->read_idx + 1) % q->max_msgs;
    q->used_msgs--;
    return 0;
}

int k_msgq_peek(struct k_msgq *q, void *data) {
    if (q->used_msgs == 0) {
        return -ENOMSG;
    }
    memcpy(data, q->buffer_start + q->read_idx * q->msg_size, q->msg_size);
    return 0;
}

void k_msgq_purge(struct k_msgq *q) { q->used_msgs = q->read_idx = q->write_idx = 0; }

/* ------------------------------------------------------------ scheduler */

static struct sim_timeout *earliest_timeout(void) {
    struct sim_timeout *best = NULL;
    for (struct sim_timeout *t = timeouts; t; t = t->sim_next) {
        if (!best || t->deadline < best->deadline) {
            best = t;
        }
    }
    return best;
}

static struct k_work_q *ready_queue(void) {
    struct k_work_q *best = NULL;
    for (struct k_work_q *q = queues; q; q = q->sim_next_q) {
        if (q->sim_head && q->sim_blocked_until <= now_ms && (!best || q->sim_prio < best->sim_prio)) {
            best = q;
        }
    }
    return best;
}

static struct k_work_q *current_q;

/* Called by a driver running on a work queue to model a blocking operation
 * (e.g. a synchronous display SPI transfer): that queue runs nothing else until `until`. */
void sim_block_current_queue_until(int64_t until) {
    if (current_q && until > current_q->sim_blocked_until) {
        current_q->sim_blocked_until = until;
    }
}

/* Tick until which the current queue is already blocked (0 if none). */
int64_t sim_block_current_queue_get(void) { return current_q ? current_q->sim_blocked_until : 0; }

static void run_one(struct k_work_q *q) {
    struct k_work *w = q->sim_head;
    q->sim_head = w->sim_next;
    if (!q->sim_head) {
        q->sim_tail = NULL;
    }
    w->flags &= ~SIM_WORK_QUEUED;
    w->flags |= SIM_WORK_RUNNING;
    current_q = q;
    w->handler(w);
    current_q = NULL;
    w->flags &= ~SIM_WORK_RUNNING;
}

/* Runs everything due at the current instant. */
static void run_now(void) {
    for (int guard = 0; guard < 100000; guard++) {
        struct sim_timeout *t = earliest_timeout();
        if (t && t->deadline <= now_ms) {
            timeout_remove(t);
            in_isr = true;
            t->fn(t);
            in_isr = false;
            continue;
        }
        struct k_work_q *q = ready_queue();
        if (q) {
            run_one(q);
            continue;
        }
        return;
    }
    sim_log(SIM_LOG_ERR, "scheduler livelock at t=%lld ticks", (long long)now_ms);
}

static int64_t sim_kernel_next_event_ticks(void) {
    int64_t next = INT64_MAX;
    struct sim_timeout *t = earliest_timeout();
    if (t) {
        next = t->deadline;
    }
    for (struct k_work_q *q = queues; q; q = q->sim_next_q) {
        if (q->sim_head) {
            next = MIN(next, MAX(q->sim_blocked_until, now_ms));
        }
    }
    return next;
}

/* host-facing API is in milliseconds */
int64_t sim_kernel_next_event(void) {
    int64_t t = sim_kernel_next_event_ticks();
    return t == INT64_MAX ? t : (int64_t)k_ticks_to_ms_ceil64(t);
}

void sim_kernel_run_until(int64_t t_ms_in) {
    int64_t t_ms = (int64_t)k_ms_to_ticks_floor64(t_ms_in); /* now in ticks */
    if (sim_reboot_requested >= 0) {
        return; /* halted until the host reboots us */
    }
    if (setjmp(reboot_jmp)) {
        reboot_armed = false;
        return;
    }
    reboot_armed = true;
    run_now();
    for (;;) {
        int64_t next = sim_kernel_next_event_ticks();
        if (next > t_ms) {
            break;
        }
        now_ms = MAX(now_ms, next);
        run_now();
    }
    now_ms = MAX(now_ms, t_ms);
    run_now();
    reboot_armed = false;
}

void sim_kernel_run_pending(void) { run_now(); }
