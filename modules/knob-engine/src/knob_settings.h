#pragma once

#include <stdint.h>

#define KNOB_KEY_SLOTS 3

/* Encoded ZMK keycode for a runtime-remappable key slot (0..2). Slots map to
 * key positions left/middle-tap/right; defaults mirror the shipped keymap. */
uint32_t knob_settings_key_code(uint8_t slot);
