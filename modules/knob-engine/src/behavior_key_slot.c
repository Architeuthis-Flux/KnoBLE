/*
 * &kslot n — key press behavior whose keycode comes from the runtime
 * settings store (raw-HID remappable, NVS-persisted) instead of the keymap.
 * Slot defaults mirror the shipped keymap, so a fresh device behaves
 * identically to one using plain &kp bindings.
 */

#define DT_DRV_COMPAT baseline_behavior_key_slot

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>

#include "knob_settings.h"

LOG_MODULE_DECLARE(knob_settings, CONFIG_KNOB_ENGINE_LOG_LEVEL);

static int on_kslot_pressed(struct zmk_behavior_binding *binding,
                            struct zmk_behavior_binding_event event) {
    const uint32_t encoded = knob_settings_key_code((uint8_t)binding->param1);
    if (encoded == 0) {
        return ZMK_BEHAVIOR_OPAQUE;
    }
    return raise_zmk_keycode_state_changed_from_encoded(encoded, true, event.timestamp);
}

static int on_kslot_released(struct zmk_behavior_binding *binding,
                             struct zmk_behavior_binding_event event) {
    const uint32_t encoded = knob_settings_key_code((uint8_t)binding->param1);
    if (encoded == 0) {
        return ZMK_BEHAVIOR_OPAQUE;
    }
    return raise_zmk_keycode_state_changed_from_encoded(encoded, false, event.timestamp);
}

static const struct behavior_driver_api kslot_driver_api = {
    .binding_pressed = on_kslot_pressed,
    .binding_released = on_kslot_released,
};

#define KSLOT_INST(n)                                                                              \
    BEHAVIOR_DT_INST_DEFINE(n, NULL, NULL, NULL, NULL, POST_KERNEL,                                \
                            CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &kslot_driver_api);

DT_INST_FOREACH_STATUS_OKAY(KSLOT_INST)
