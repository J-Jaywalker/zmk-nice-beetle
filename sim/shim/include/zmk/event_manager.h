/*
 * Wraps ZMK's real <zmk/event_manager.h>.  Only the two macros that place
 * objects in linker sections are redefined so that, on Windows/PE, the
 * subscriptions are laid out contiguously *in the same link order as the real
 * firmware* (SIM_LINK_ORDER is the file's position in the real CMake source
 * list).  ZMK's event_manager.c itself is compiled unmodified.
 */
#pragma once
#include_next <zmk/event_manager.h>
#include <zephyr/sys/iterable_sections.h>

#define SIM_STR2(x) #x
#define SIM_STR(x) SIM_STR2(x)

#undef ZMK_EVENT_IMPL
#define ZMK_EVENT_IMPL(event_type)                                                                 \
    const struct zmk_event_type zmk_event_##event_type = {.name = STRINGIFY(event_type)};          \
    const struct zmk_event_type *zmk_event_ref_##event_type __used                                 \
        __attribute__((__section__(".zsim$event_type_b"))) = &zmk_event_##event_type;             \
    struct event_type##_event copy_raised_##event_type(const struct event_type *ev) {              \
        struct event_type##_event *outer = CONTAINER_OF(ev, struct event_type##_event, data);      \
        return *outer;                                                                             \
    };                                                                                             \
    int raise_##event_type(struct event_type data) {                                               \
        struct event_type##_event ev = {.data = data,                                              \
                                        .header = {.event = &zmk_event_##event_type}};             \
        return ZMK_EVENT_RAISE(ev);                                                                \
    };                                                                                             \
    struct event_type *as_##event_type(const zmk_event_t *eh) {                                    \
        /* eh == NULL: on nRF52 address 0 is readable flash (vector table, initial SP), so the   \
         * real code reads a value that never matches and returns NULL; reproduce that. */       \
        return (eh != NULL && eh->event == &zmk_event_##event_type)                                \
                   ? &((struct event_type##_event *)eh)->data                                      \
                   : NULL;                                                                         \
    };

#undef ZMK_SUBSCRIPTION
#define ZMK_SUBSCRIPTION(mod, ev_type)                                                             \
    extern const struct zmk_listener zmk_listener_##mod;                                           \
    const Z_DECL_ALIGN(struct zmk_event_subscription)                                              \
        _CONCAT(_CONCAT(zmk_event_sub_, mod), ev_type) __used __attribute__((                      \
            __section__(".zsim$event_subscription_b_" SIM_STR(SIM_LINK_ORDER)))) = {               \
            .event_type = &zmk_event_##ev_type,                                                    \
            .listener = &zmk_listener_##mod,                                                       \
    };
