#include "button.h"
#include <string.h>

void button_init(btn_state_t *b, uint32_t hold_ms, uint32_t double_ms)
{
    memset(b, 0, sizeof(*b));
    b->hold_ms = hold_ms;
    b->double_ms = double_ms;
}

btn_evt_t button_feed(btn_state_t *b, bool pressed, uint32_t now_ms)
{
    /* --- press edge --- */
    if (pressed && !b->down) {
        b->down = true;
        b->press_ms = now_ms;
        b->hold_fired = false;
        if (b->tap_pending && (now_ms - b->tap_ms) <= b->double_ms) {
            /* second press inside the window: report it immediately, and make
             * sure the release that follows does not also emit a single tap */
            b->tap_pending = false;
            b->swallow_tap = true;
            return BTN_DOUBLE;
        }
        return BTN_NONE;
    }

    /* --- held long enough --- */
    if (pressed && b->down && !b->hold_fired && (now_ms - b->press_ms) >= b->hold_ms) {
        b->hold_fired = true;
        b->tap_pending = false;      /* a hold is never part of a double-tap */
        b->swallow_tap = false;
        return BTN_HOLD;
    }

    /* --- release edge --- */
    if (!pressed && b->down) {
        b->down = false;
        if (b->hold_fired || b->swallow_tap) {
            b->swallow_tap = false;  /* consumed by HOLD or DOUBLE */
        } else {
            b->tap_pending = true;   /* wait: a second press may still arrive */
            b->tap_ms = now_ms;
        }
        return BTN_NONE;
    }

    /* --- the double-tap window expired: it really was a single tap --- */
    if (b->tap_pending && (now_ms - b->tap_ms) > b->double_ms) {
        b->tap_pending = false;
        return BTN_TAP;
    }
    return BTN_NONE;
}
