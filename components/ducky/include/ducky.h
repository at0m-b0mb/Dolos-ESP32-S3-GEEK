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
#include "layout.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DUCKY_KEY = 0,   /* tap key with a.mods (plus any held mods)   */
    DUCKY_DELAY,     /* wait a.delay_ms                            */
    DUCKY_MOUSE,     /* mouse move/click/wheel                     */
    DUCKY_CONSUMER,  /* media / consumer usage                     */
    DUCKY_HOLD,      /* press+HOLD a.mods across following keys     */
    DUCKY_RELEASE,   /* release all held modifiers                 */
    DUCKY_WAIT,      /* block until a host lock-key condition holds  */
    DUCKY_SPECIAL    /* device action; see ducky_special_t in a.special */
} ducky_action_kind_t;

/* Device-level commands. They do not type anything, so the player carries them
 * out itself: the interpreter and the linter only have to agree they exist. */
typedef enum {
    DSP_NONE = 0,
    DSP_LED_OFF, DSP_LED_R, DSP_LED_G,      /* the GEEK has a screen, not an RGB LED */
    DSP_SAVE_LOCKS, DSP_RESTORE_LOCKS,      /* host CAPS/NUM/SCROLL state             */
    DSP_WAIT_BUTTON,                        /* halt until BOOT is pressed             */
    DSP_BUTTON_ENABLE, DSP_BUTTON_DISABLE,
    DSP_ATTACKMODE_HID, DSP_ATTACKMODE_OFF, /* STORAGE is refused, loudly              */
    DSP_SAVE_ATTACKMODE, DSP_RESTORE_ATTACKMODE,
    DSP_EXFIL,                              /* append a value to the loot file         */
    DSP_NOP                                 /* accepted and deliberately does nothing  */
} ducky_special_t;

/* Target OS for the Unicode "type anything" input method. */
typedef enum { OS_WINDOWS = 0, OS_LINUX, OS_MAC } target_os_t;

typedef struct {
    uint8_t  kind;      /* ducky_action_kind_t                       */
    uint8_t  mods;      /* HID_MOD_* bitmask (KEY)                    */
    uint8_t  key;       /* HID usage id, 0 = modifier-only chord      */
    uint32_t delay_ms;  /* DELAY                                      */
    int8_t   mx, my;    /* MOUSE relative move                        */
    int8_t   wheel;     /* MOUSE wheel                                */
    uint8_t  buttons;   /* MOUSE buttons bitmap (1=L,2=R,4=M); click  */
    uint16_t consumer;  /* CONSUMER (media) usage code                */
    uint8_t  wait_mask; /* WAIT: which lock LED (HID_LED_*)           */
    uint8_t  wait_want; /* WAIT: 0=off 1=on 2=change                  */
    uint8_t  special;   /* SPECIAL: ducky_special_t                   */
    char     text[40];  /* SPECIAL: EXFIL payload / mode argument      */
} ducky_action_t;

typedef struct {
    uint32_t    default_delay_ms;/* inserted between commands (DEFAULTDELAY) */
    char        last_cmd[512];    /* REPEAT target: a command, not a document */
    char        scratch[8192];   /* parse buffer, per caller - never on the stack */   /* remembered for REPEAT                    */
    int         repeat;          /* pending REPEAT count (player consumes)   */
    kb_layout_t layout;          /* target keyboard layout for STRING/chars  */
    target_os_t target_os;       /* OS for Unicode (STRING non-ASCII/UNICODE)*/
    uint32_t    string_delay_ms;  /* extra pause BETWEEN characters of a STRING */
    bool        in_rem_block;     /* inside REM_BLOCK ... END_REM               */
    uint32_t    rng_state;        /* RANDOM_*: deterministic unless reseeded    */
} ducky_state_t;

void ducky_state_init(ducky_state_t *st);

/* Parse ONE line into up to `max` actions. Returns the count written (may be 0
 * for comments / DEFAULTDELAY). For REPEAT n, returns 0 and sets st->repeat=n:
 * the player replays st->last_cmd n more times. */
/* Host LED bits, as reported back over HID OUT. */
#define HID_LED_NUMLOCK  0x01
#define HID_LED_CAPSLOCK 0x02
#define HID_LED_SCROLL   0x04

/* Caps Lock lives in the OPERATING SYSTEM, not in the keyboard, so a payload
 * that types "Hello" into a machine with Caps Lock on gets "hELLO" - the case
 * of every letter is inverted. Rather than toggling the host's lock state
 * (which changes something we were not asked to change, and leaves it changed
 * if the payload aborts), flip the shift bit for A-Z so the text arrives
 * exactly as written. Returns the modifiers to send for this key. */
uint8_t ducky_apply_caps(uint8_t key, uint8_t mods, uint8_t leds);

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
