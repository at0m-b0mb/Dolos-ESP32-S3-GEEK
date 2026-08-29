/*
 * ducky.h - a DuckyScript-style payload parser for authorized lab use.
 *
 * Pure C, no hardware, no allocation. It turns one script line into a small
 * list of HID actions (a key chord to press+release, or a delay), which the
 * device player then emits over USB HID. Because it is pure, test/host exercises
 * the whole language on a laptop before it ever drives a real keyboard.
 *
 * Supported: REM/#, STRING, STRINGLN, DELAY, DEFAULTDELAY/DEFAULT_DELAY, REPEAT,
 * modifier combos (CTRL/CONTROL, ALT, SHIFT, GUI/WINDOWS/COMMAND) plus named
 * keys (ENTER, TAB, ESC, SPACE, arrows, F1..F12, DELETE, HOME, END, ...).
 */
#ifndef DOLOS_DUCKY_H
#define DOLOS_DUCKY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { DUCKY_KEY = 0, DUCKY_DELAY } ducky_action_kind_t;

typedef struct {
    uint8_t  kind;      /* ducky_action_kind_t                       */
    uint8_t  mods;      /* HID_MOD_* bitmask (KEY)                    */
    uint8_t  key;       /* HID usage id, 0 = modifier-only chord      */
    uint32_t delay_ms;  /* DELAY                                      */
} ducky_action_t;

typedef struct {
    uint32_t default_delay_ms;   /* inserted between commands (DEFAULTDELAY) */
    char     last_cmd[160];      /* remembered for REPEAT                    */
    int      repeat;             /* pending REPEAT count (player consumes)   */
} ducky_state_t;

void ducky_state_init(ducky_state_t *st);

/* Parse ONE line into up to `max` actions. Returns the count written (may be 0
 * for comments / DEFAULTDELAY). For REPEAT n, returns 0 and sets st->repeat=n:
 * the player replays st->last_cmd n more times. */
int ducky_parse_line(ducky_state_t *st, const char *line,
                     ducky_action_t *out, int max);

/* Exposed for tests + the keymap. US layout. */
bool hid_from_ascii(char c, uint8_t *key, uint8_t *mods);
bool hid_named_key(const char *tok, uint8_t *key);   /* ENTER, UP, F5, ... */
bool hid_modifier(const char *tok, uint8_t *modbit); /* CTRL, ALT, SHIFT, GUI */

#ifdef __cplusplus
}
#endif
#endif /* DOLOS_DUCKY_H */
