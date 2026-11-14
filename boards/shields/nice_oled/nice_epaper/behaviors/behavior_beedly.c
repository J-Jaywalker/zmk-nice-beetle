/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * `&beedly` behaviour (compatible "zmk,behavior-beedly", one parameter from
 * <dt-bindings/zmk/beedly.h>). The same driver is built into both halves:
 *
 *   central (left)      BEEDLY_OPEN / UP / DOWN / ENTER from the keymap: the menu cursor and the
 *                       menu layer (widgets/beedly_menu.c). ENTER sends BEEDLY_PET / FEED / SAVE
 *                       to the peripheral with zmk_split_central_invoke_behavior(), using this
 *                       behaviour's own device name, so the peripheral runs this driver too.
 *   peripheral (right)  BEEDLY_PET / FEED / SAVE: the pet (widgets/beedly_pet.c). The event the
 *                       peripheral gets carries only position and timestamp; nothing here reads
 *                       event.layer or event.source.
 *
 * Key presses only; releases do nothing. Callbacks only change plain state and submit work, the
 * screens are drawn on the display work queue. Any other parameter fails with -ENOTSUP.
 */

#define DT_DRV_COMPAT zmk_behavior_beedly

#include <zephyr/device.h>
#include <drivers/behavior.h>
#include <zephyr/logging/log.h>

#include <dt-bindings/zmk/beedly.h>
#include <zmk/behavior.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

#include <zmk/split/central.h>
#include "../widgets/beedly_menu.h"

/* Split source id of the (only) peripheral: the right half */
#define BEEDLY_PERIPHERAL_SOURCE 0

static int send_to_peripheral(const struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event, uint32_t action) {
    struct zmk_behavior_binding remote = *binding; /* same device name on both halves */
    remote.param1 = action;
    remote.param2 = 0;

    // Queued as a write-without-response: 0 means queued, not delivered. Errors (link down,
    // no transport: -ENODEV / -ENOTCONN ...) are logged and otherwise ignored.
    int err = zmk_split_central_invoke_behavior(BEEDLY_PERIPHERAL_SOURCE, &remote, event, true);
    if (err < 0) {
        LOG_WRN("beedly: sending action %u to the right half failed (%d)", (unsigned int)action,
                err);
    }
    return err;
}

static int on_beedly_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    uint32_t action;

    switch (binding->param1) {
    case BEEDLY_OPEN:
        beedly_menu_open();
        return ZMK_BEHAVIOR_OPAQUE;
    case BEEDLY_UP:
        beedly_menu_move(-1);
        return ZMK_BEHAVIOR_OPAQUE;
    case BEEDLY_DOWN:
        beedly_menu_move(1);
        return ZMK_BEHAVIOR_OPAQUE;
    case BEEDLY_ENTER:
        if (!beedly_menu_selected_action(&action)) {
            return ZMK_BEHAVIOR_OPAQUE; /* menu not open */
        }
        send_to_peripheral(binding, event, action);
        if (action == BEEDLY_SAVE) {
            // KNOWN ISSUE (TODO.md): the menu closes even if the send failed or was dropped, so
            // with the right half out of range SAVE silently does nothing. The right half still
            // saves by itself (30 s after an action, on idle, before sleep). A failed send could
            // keep the menu open (send_to_peripheral() < 0), but a queued BLE write-without-
            // response is never acknowledged, so that would only cover part of the cases.
            beedly_menu_close(); /* PET / FEED keep the menu open */
        }
        return ZMK_BEHAVIOR_OPAQUE;
    default:
        LOG_WRN("beedly: parameter %u is not a central (keymap) parameter",
                (unsigned int)binding->param1);
        return -ENOTSUP;
    }
}

#else /* peripheral */

#include "../widgets/beedly_pet.h"

static int on_beedly_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    switch (binding->param1) {
    case BEEDLY_PET:
    case BEEDLY_FEED:
    case BEEDLY_SAVE:
        return beedly_pet_request(binding->param1) < 0 ? -EIO : ZMK_BEHAVIOR_OPAQUE;
    default:
        LOG_WRN("beedly: parameter %u is not a remote (right half) action",
                (unsigned int)binding->param1);
        return -ENOTSUP;
    }
}

#endif /* IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL) */

static int on_beedly_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

/* The keymap-facing parameters only (BEEDLY_PET/FEED/SAVE are never bound in a keymap) */
static const struct behavior_parameter_value_metadata param_values[] = {
    {.display_name = "Open menu", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = BEEDLY_OPEN},
    {.display_name = "Up", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = BEEDLY_UP},
    {.display_name = "Down", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = BEEDLY_DOWN},
    {.display_name = "Enter", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = BEEDLY_ENTER},
};

static const struct behavior_parameter_metadata_set param_metadata_set[] = {{
    .param1_values = param_values,
    .param1_values_len = ARRAY_SIZE(param_values),
}};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(param_metadata_set),
    .sets = param_metadata_set,
};

#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_driver_api behavior_beedly_driver_api = {
    // keymap bindings run on the central only; the peripheral gets explicit remote actions
    .locality = BEHAVIOR_LOCALITY_CENTRAL,
    .binding_pressed = on_beedly_binding_pressed,
    .binding_released = on_beedly_binding_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_beedly_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
