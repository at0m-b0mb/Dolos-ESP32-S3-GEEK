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
#include <stddef.h>
#include <stdint.h>

/* Parse buffers are kilobytes each, and internal RAM on this chip is what
 * Wi-Fi, TinyUSB and the HTTP server compete for - spending it here is what
 * crashed the device earlier in this project. On the ESP32 they live in PSRAM;
 * on a host they are ordinary statics. */
#define DOLOS_BIG_BSS   /* see dscript_alloc(): these live on the PSRAM heap */

#ifdef __cplusplus
extern "C" {
#endif

#define DS_MAX_LINES  1400   /* lines in a payload                        */
#define DS_MAX_VARS     32   /* VAR entries (numeric)                     */
#define DS_MAX_DEFS     40   /* DEFINE entries (text macros)              */
#define DS_DEF_NAME     40   /* $_HOST_CONFIGURATION_REQUEST_COUNT is 33  */
#define DS_DEF_VAL     128   /* a URL or a webhook, comfortably           */
#define DS_MAX_FUNCS    16   /* FUNCTION definitions                      */
#define DS_MAX_DEPTH    16   /* nesting of calls and loops                */
/* Runaway guard. Sized from the official library: the longest legitimate
 * payload there executes about 171,000 lines, so this leaves headroom while
 * still stopping a loop with no exit. A payload that means to run for ever
 * (a menu, a prank) is aborted at the device instead. */
#define DS_MAX_STEPS 1000000
#define DS_LINE_MAX   8192   /* matches the player's line buffer          */

typedef struct {
    const char *text;

    struct { uint16_t off, len; } line[DS_MAX_LINES];
    uint16_t nlines;
    uint16_t pc;                       /* next line to consider           */

    struct { char name[DS_DEF_NAME]; int32_t val; } var[DS_MAX_VARS];

    /* DEFINE is a TEXT MACRO, not a number: the official payloads write
     *     DEFINE #SCRIPT_URL https://example.com/script.ps1
     * and expect the name replaced by that text wherever it appears. Treating
     * it as an arithmetic assignment failed 136 of the 253 official payloads,
     * because almost no DEFINE in the library holds a number. */
    struct { char name[DS_DEF_NAME]; char val[DS_DEF_VAL]; } def[DS_MAX_DEFS];
    uint8_t ndefs;
    uint8_t nvars;

    struct { char name[DS_DEF_NAME]; uint16_t line; } fn[DS_MAX_FUNCS];
    uint8_t nfns;

    uint16_t ret[DS_MAX_DEPTH];  uint8_t nret;    /* return addresses      */
    /* Where a function's RETURN value should be stored, per call frame.
     * "$X = FUNC()" is how the official payloads capture a result; the call
     * runs as a normal statement and RETURN assigns into this. */
    char     ret_var[DS_MAX_DEPTH][DS_DEF_NAME];
    uint16_t loop[DS_MAX_DEPTH]; uint8_t nloop;   /* WHILE line numbers    */

    uint8_t  block;        /* inside STRING/END_STRING (1) or STRINGLN (2)   */
    /* Host facts the script can read as $_ system variables. The caller keeps
     * these current; the interpreter never touches hardware itself. */
    int32_t  host_os;      /* 0 Windows, 1 Linux, 2 macOS                    */
    uint8_t  host_leds;    /* live lock-key LEDs: bit0 Num, bit1 Caps, bit2 Scroll */
    uint8_t  saved_leds;   /* SAVE_HOST_KEYBOARD_LOCK_STATE                  */
    uint8_t  button_pushed;
    /* Facts a payload uses to work out what it is plugged into. Windows, Linux
     * and macOS ask for USB descriptors a different number of times, and only
     * some hosts send a lock-LED report back - which is why the official
     * OS-detection payloads read exactly these two. */
    int32_t  host_cfg_requests;
    uint8_t  host_lock_reply;
    uint32_t (*rnd)(void); /* random source for $_RANDOM_INT (NULL = counter) */
    uint32_t rnd_ctr;
    uint32_t steps;
    /* +16: a block line is handed back with a "STRINGLN " prefix, so the
     * output is legitimately longer than the input line it came from. */
    char     out[DS_LINE_MAX + 16];
    /* Two more line-sized scratch buffers that MUST NOT be locals.
     *
     * dscript_next() held two of these on the stack and match_end() a third,
     * giving a 16.5 KB frame that calls an 8.3 KB one - about 24.7 KB, on a
     * task with 6 KB. It smashed the stack on any payload whose control flow
     * reached that path, which presented as the device freezing and then being
     * reset by the watchdog. They live in the struct, which is on the PSRAM
     * heap, so the frames become a few dozen bytes.
     *
     * cond: the IF / ELSE IF condition being evaluated. Safe to share between
     *       the two, because the first is finished with before the chain walk
     *       begins.
     * scan: match_end()'s line buffer, separate because it is used WHILE cond
     *       and work are both live. */
    char     cond[DS_LINE_MAX];
    char     scan[DS_LINE_MAX];
    char     work[DS_LINE_MAX];        /* line scratch - NOT on the stack */         /* expanded line handed to the caller */
    const char *err;                   /* non-NULL once the script is broken */
    /* Big enough for the longest message plus a full-length name, so an
     * error can always say WHICH thing it is about. */
    char     errbuf[DS_DEF_NAME + 56];
    uint16_t err_line;                 /* 1-based line the error came from   */
} dscript_t;

/* One shared interpreter instance, allocated from external RAM on the device
 * and from the ordinary heap on a host. Callers use this rather than declaring
 * a dscript_t: the struct carries kilobytes of line buffers, which belong on
 * neither a task stack nor in the internal RAM that Wi-Fi and USB compete for.
 * Returns NULL only if the allocation fails, which the caller must handle. */
dscript_t *dscript_alloc(void);

/* The shared interpreter. 31 KB each, and never more than one in use at a
 * time, so everything that needs one takes it from here. */
dscript_t *dscript_shared(void);

/* Large scratch buffers: external RAM on the device, plain memory on the host.
 * Allocated once and kept - never freed, never on a task stack. */
void *ducky_big_alloc(size_t n);

/* Small buffers touched per character: internal RAM on the device. */
void *ducky_hot_alloc(size_t n);

/* Installed by the firmware so long parses can let other tasks run. Without
 * one, a big payload can hold the CPU past the task-watchdog timeout. */
void ducky_set_yield(void (*fn)(void));
void ducky_yield(void);

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

/* USB-level facts the OS-detection payloads read. */
void dscript_set_host_usb(dscript_t *ds, int32_t cfg_requests, uint8_t lock_reply);

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
