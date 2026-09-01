/*
 * dscript.h - the DuckyScript 3 programming layer.
 *
 * ducky.c turns ONE line into keystrokes. That is all a payload needed while it
 * was a straight list of commands, but DuckyScript 3 has variables, conditions,
 * loops and functions, and none of those can be understood a line at a time:
 * IF has to know where its END_IF is, WHILE has to jump backwards, a function
 * call has to come back.
 *
 * So this sits above the line parser and decides WHICH line runs next. It
 * consumes the control-flow lines itself and hands back only the lines that
 * actually type something - with variables already substituted. The player
 * therefore stays exactly as simple as it was:
 *
 *     dscript_init(&ds, text);
 *     while ((line = dscript_next(&ds)) != NULL) {
 *         n = ducky_parse_line(&st, line, acts, max);
 *         play(acts, n);
 *     }
 *
 * Pure C, no allocation, fixed bounds, and no hardware anywhere near it, so the
 * whole language is exercised on a laptop.
 *
 * Everything is bounded on purpose: an embedded device must not be brought down
 * by a payload with a runaway loop or five thousand lines, so every limit is a
 * refusal with a message rather than a crash.
 */
#ifndef DOLOS_DSCRIPT_H
#define DOLOS_DSCRIPT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DS_MAX_LINES  1400   /* lines in a payload                        */
#define DS_MAX_VARS     32   /* VAR + DEFINE entries                      */
#define DS_MAX_FUNCS    16   /* FUNCTION definitions                      */
#define DS_MAX_DEPTH    16   /* nesting of calls and loops                */
#define DS_MAX_STEPS 200000  /* runaway guard: a loop cannot run for ever */
#define DS_LINE_MAX   2048   /* matches the player's line buffer          */

typedef struct {
    const char *text;

    struct { uint16_t off, len; } line[DS_MAX_LINES];
    uint16_t nlines;
    uint16_t pc;                       /* next line to consider           */

    struct { char name[16]; int32_t val; } var[DS_MAX_VARS];
    uint8_t nvars;

    struct { char name[20]; uint16_t line; } fn[DS_MAX_FUNCS];
    uint8_t nfns;

    uint16_t ret[DS_MAX_DEPTH];  uint8_t nret;    /* return addresses      */
    uint16_t loop[DS_MAX_DEPTH]; uint8_t nloop;   /* WHILE line numbers    */

    uint8_t  block;        /* inside STRING/END_STRING (1) or STRINGLN (2)   */
    /* Host facts the script can read as $_ system variables. The caller keeps
     * these current; the interpreter never touches hardware itself. */
    int32_t  host_os;      /* 0 Windows, 1 Linux, 2 macOS                    */
    uint8_t  host_leds;    /* live lock-key LEDs: bit0 Num, bit1 Caps, bit2 Scroll */
    uint8_t  saved_leds;   /* SAVE_HOST_KEYBOARD_LOCK_STATE                  */
    uint8_t  button_pushed;
    uint32_t (*rnd)(void); /* random source for $_RANDOM_INT (NULL = counter) */
    uint32_t rnd_ctr;
    uint32_t steps;
    /* +16: a block line is handed back with a "STRINGLN " prefix, so the
     * output is legitimately longer than the input line it came from. */
    char     out[DS_LINE_MAX + 16];         /* expanded line handed to the caller */
    const char *err;                   /* non-NULL once the script is broken */
    uint16_t err_line;                 /* 1-based line the error came from   */
} dscript_t;

/* Index the text and pre-scan FUNCTION definitions. false if it is unusable. */
bool dscript_init(dscript_t *ds, const char *text);

/* The next line that actually types something, with $variables substituted, or
 * NULL when the payload is finished (or has failed - check dscript_error). */
const char *dscript_next(dscript_t *ds);

/* NULL unless the script failed; then a short human-readable reason. */
const char *dscript_error(const dscript_t *ds);
uint16_t    dscript_error_line(const dscript_t *ds);

/* Read a variable back, mainly so tests can assert on what a script computed. */
bool dscript_get(const dscript_t *ds, const char *name, int32_t *out);

/* Tell the script about the machine it is running on, so $_OS, $_CAPSLOCK_ON,
 * $_RANDOM_INT and friends mean something. Safe to call between lines. */
void dscript_set_host(dscript_t *ds, int32_t os, uint8_t leds, uint8_t button_pushed);

/* True if the line is a control-flow KEYWORD (no context needed). */
bool dscript_is_control(const char *line);

/* True if this layer consumes the line at all: a keyword, an assignment like
 * "$i = $i + 1", or a call to a FUNCTION defined in this script. The linter
 * uses it so that the two never disagree about what counts as a command -
 * previously it knew the keywords but not assignments or calls, and reported
 * perfectly valid lines as unknown commands. */
bool dscript_is_consumed(const dscript_t *ds, const char *line);

#ifdef __cplusplus
}
#endif
#endif /* DOLOS_DSCRIPT_H */
