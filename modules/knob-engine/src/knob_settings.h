#pragma once

#include <stdint.h>

#define KNOB_KEY_SLOTS 3

/* Encoded ZMK keycode for a runtime-remappable key slot (0..2). Slots map to
 * key positions left/middle-tap/right; defaults mirror the shipped keymap. */
uint32_t knob_settings_key_code(uint8_t slot);

/* Slide pot roles. Each role keeps its own sensitivity — switching roles
 * never resets the other role's dial. */
enum knob_pot_role {
    KNOB_POT_ROLE_SPEED = 0,   /* scales scroll speed (divider..multiplier) */
    KNOB_POT_ROLE_HSCROLL = 1, /* its own control: horizontal wheel steps */
    KNOB_POT_ROLE_VOLUME = 2,  /* its own control: volume up/down steps */
    KNOB_POT_ROLE_OFF = 3,
};

struct knob_pot_cfg {
    uint8_t role;
    /* KNOB_POT_ROLE_SPEED sensitivity: range /min_div .. x max_mult */
    uint8_t speed_max_mult;
    uint8_t speed_min_div;
    /* own-control roles' sensitivity: bucket count over the pot's travel */
    uint8_t steps;
};

const struct knob_pot_cfg *knob_settings_pot(void);

/* Engine -> settings: latest pot sample, for live reporting to the host.
 * semantic: signed speed for the speed role, bucket index otherwise. */
void knob_settings_note_pot(int32_t raw, int16_t semantic);
