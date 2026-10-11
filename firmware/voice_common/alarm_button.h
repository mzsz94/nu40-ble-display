/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WALLABY_ALARM_BUTTON_H
#define WALLABY_ALARM_BUTTON_H
#include <stdbool.h>
#include <stdint.h>

struct alarm_button_state {
    bool raw;
    bool stable;
    uint32_t changed_at;
};

/* Return exactly one event after 30 ms of stable press, including clock wrap. */
static inline bool alarm_button_press(struct alarm_button_state *state, bool pressed,
                                      uint32_t now)
{
    if (pressed != state->raw) {
        state->raw = pressed;
        state->changed_at = now;
    }
    if (state->raw != state->stable && (uint32_t)(now - state->changed_at) >= 30) {
        state->stable = state->raw;
        return state->stable;
    }
    return false;
}
#endif
