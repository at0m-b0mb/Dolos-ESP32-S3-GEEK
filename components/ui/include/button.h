/*
 * button.h - one-button gesture recognizer (tap / double-tap / hold).
 *
 * The GEEK has no touchscreen and exactly one button, so every interaction has
 * to come out of three gestures. Getting that right matters: a mis-detected
 * hold on a device that types into someone else's computer is not a cosmetic
 * bug. So the recognizer is a pure function of (pressed, now_ms) with no GPIO
 * and no clock of its own, and the host tests drive it through real timelines.
 *
 * Semantics:
 *   HOLD    fires the moment the press passes hold_ms, while still held.
 *   DOUBLE  fires on the SECOND press when it starts within double_ms.
 *   TAP     is deferred by double_ms - it only fires once a second press can
 *           no longer arrive. That delay is what makes double-tap possible.
 */
#ifndef DOLOS_BUTTON_H
#define DOLOS_BUTTON_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { BTN_NONE = 0, BTN_TAP, BTN_DOUBLE, BTN_HOLD } btn_evt_t;

typedef struct {
    uint32_t hold_ms;      /* press longer than this = HOLD          */
    uint32_t double_ms;    /* second press within this = DOUBLE      */
    bool     down;
    bool     hold_fired;
    bool     swallow_tap;  /* this release completed a double-tap    */
    bool     tap_pending;
    uint32_t press_ms;
    uint32_t tap_ms;
} btn_state_t;

void      button_init(btn_state_t *b, uint32_t hold_ms, uint32_t double_ms);
btn_evt_t button_feed(btn_state_t *b, bool pressed, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
#endif /* DOLOS_BUTTON_H */
