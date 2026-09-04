#include "payload.h"
#include "usb_hid.h"
#include "ducky.h"
#include "dscript.h"
#include "hid_keys.h"
#include "usb_msc.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include <stdarg.h>

static const char *TAG = "payload";

/* Why the last run did nothing. A run that types zero keystrokes and is then
 * recorded as "sent" is worse than a crash: on an engagement the audit log is
 * the evidence, and a silent no-op that looks like a success is a lie in it. */
static char s_fail[64];
void payload_set_fail(const char *why)
{
    snprintf(s_fail, sizeof(s_fail), "%s", why ? why : "");
}
const char *payload_last_failure(void) { return s_fail[0] ? s_fail : NULL; }

/* ---- injection log -------------------------------------------------------
 * Every keystroke, exactly as it was attempted: the HID usage id, the
 * modifiers actually sent, how many retries the host needed, and the elapsed
 * time. If characters go missing this says which ones and why - whether the
 * report was refused, retried, or accepted and still not typed (which points
 * at the host, not at us). Written to /sdcard/DOLOS_INJECT.LOG. */
static FILE *s_ilog;
static uint32_t s_ilog_t0;

static uint32_t ms_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static char printable_of(uint8_t key, uint8_t mods)
{
    if (key >= HID_KEY_A && key <= HID_KEY_A + 25) {
        char base = (char)('a' + (key - HID_KEY_A));
        return (mods & HID_MOD_LSHIFT) ? (char)(base - 32) : base;
    }
    if (key == HID_KEY_SPACE) return '_';
    if (key >= HID_KEY_1 && key <= HID_KEY_1 + 8)
        return (char)('1' + (key - HID_KEY_1));
    if (key == HID_KEY_0) return '0';
    return '.';
}

static void ilog_open(const char *name, const payload_ctx_t *ctx)
{
    s_ilog = fopen("/sdcard/DOLOS_INJECT.LOG", "a");
    if (!s_ilog) return;
    /* A real buffer, and NO flush per keystroke.
     *
     * Every logged keystroke used to force an SD card write - a card
     * transaction between one character and the next, on the task that is
     * meant to be typing. That is the slowest thing in the injection path and
     * it sits exactly where timing matters most. Buffered, the log costs
     * almost nothing and is written in whole blocks; it is still closed
     * properly at the end of the run, so nothing is lost. */
    static char ilog_buf[2048];
    setvbuf(s_ilog, ilog_buf, _IOFBF, sizeof(ilog_buf));
    s_ilog_t0 = ms_now();
    fprintf(s_ilog,
        "\n===== injection: payload=%s layout=%d os=%d dry=%d =====\n"
        "  idx    ms  line  key  mods  ch  retries  result\n",
        name ? name : "?", (int)ctx->layout, (int)ctx->os, ctx->dry_run ? 1 : 0);
    fflush(s_ilog);
}

static void ilog_key(int idx, int line, uint8_t key, uint8_t mods)
{
    if (!s_ilog) return;
    fprintf(s_ilog, "%5d %5lu %5d  0x%02X  0x%02X   %c  %7u  %s\n",
            idx, (unsigned long)(ms_now() - s_ilog_t0), line, key, mods,
            printable_of(key, mods), usb_hid_last_retries(),
            usb_hid_last_ok() ? "sent" : "DROPPED");
    /* deliberately not flushed: see ilog_open() */
}

static void ilog_note(const char *fmt, ...)
{
    if (!s_ilog) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(s_ilog, fmt, ap);
    va_end(ap);
    fflush(s_ilog);
}

static void ilog_close(int keys, uint32_t drops_before)
{
    if (!s_ilog) return;
    fprintf(s_ilog, "----- %d keystroke(s), %lu dropped by the host, %lu ms -----\n",
            keys, (unsigned long)(usb_hid_drops() - drops_before),
            (unsigned long)(ms_now() - s_ilog_t0));
    fclose(s_ilog);
    s_ilog = NULL;
}

const char DOLOS_DEMO_PAYLOAD[] =
    "REM Dolos default demo - authorized lab use only\n"
    "REM Types a banner into whatever text field has focus. No launch commands.\n"
    "DEFAULTDELAY 40\n"
    "DELAY 800\n"
    "STRING Dolos BadUSB demo - authorized lab use only.\n"
    "ENTER\n"
    "STRING If you can read this line, HID keystroke injection works.\n"
    "ENTER\n";

int payload_count_lines(const char *text)
{
    int n = 0; bool any = false;
    for (const char *p = text; *p; p++) { any = true; if (*p == '\n') n++; }
    if (any && text[strlen(text) - 1] != '\n') n++;   /* last line w/o newline */
    return n;
}

const char *payload_load(char *buf, int cap)
{
    FILE *fp = fopen("/sdcard/PAYLOAD.TXT", "r");
    if (!fp) { ESP_LOGI(TAG, "no /sdcard/PAYLOAD.TXT - using built-in demo"); return DOLOS_DEMO_PAYLOAD; }
    int n = (int)fread(buf, 1, cap - 1, fp);
    fclose(fp);
    if (n <= 0) return DOLOS_DEMO_PAYLOAD;
    buf[n] = 0;
    ESP_LOGI(TAG, "loaded %d bytes from /sdcard/PAYLOAD.TXT", n);
    return buf;
}

/* The Windows Unicode method types on the NUMERIC KEYPAD while Alt is held, and
 * the keypad only produces digits when Num Lock is ON. With it off, every
 * non-ASCII character silently produces nothing. Turn it on before the first
 * such sequence; the host owns this state, so we only ever switch it on when it
 * is needed and never toggle it back and forth mid-payload. */
static void ensure_numlock(const payload_ctx_t *ctx)
{
    if (ctx->dry_run || ctx->os != OS_WINDOWS) return;
    if (usb_hid_leds() & HID_LED_NUMLOCK) return;
    usb_hid_tap(0, HID_KEY_NUMLOCK);
    vTaskDelay(pdMS_TO_TICKS(40));            /* let the host apply it */
}

/* Device-level state a payload can touch. */
uint8_t  g_saved_leds;      /* SAVE_HOST_KEYBOARD_LOCK_STATE                */
bool     g_saved_locks;
volatile bool g_wait_button;/* WAIT_FOR_BUTTON_PRESS - cleared by the UI task */
uint8_t  g_payload_led;     /* LED_R / LED_G / LED_OFF, shown on the screen  */

/* EXFIL appends to a loot file on the card. Skipped while the host holds the
 * storage window, because writing under it would corrupt the filesystem the
 * host is using. */
static void exfil_write(const char *text)
{
    if (usb_msc_exposed()) { ilog_note("  EXFIL skipped: host holds the storage\n"); return; }
    FILE *fp = fopen("/sdcard/LOOT.TXT", "a");
    if (!fp) { ilog_note("  EXFIL failed: no card\n"); return; }
    fprintf(fp, "%s\n", text ? text : "");
    fclose(fp);
    ilog_note("  EXFIL wrote %u bytes\n", (unsigned)(text ? strlen(text) : 0));
}

/* Put the host's lock keys back the way they were. The locks live in the OS,
 * so the only way to change them is to press the keys - and the only way to
 * know the result is the LED report coming back. */
static void restore_locks(const payload_ctx_t *ctx)
{
    const struct { uint8_t bit; uint8_t key; } L[] = {
        { HID_LED_CAPSLOCK, HID_KEY_CAPS },
        { HID_LED_NUMLOCK,  HID_KEY_NUMLOCK },
        { HID_LED_SCROLL,   HID_KEY_SCROLLLOCK },
    };
    for (int i = 0; i < 3; i++) {
        if (ctx->abort && *ctx->abort) return;
        uint8_t now = usb_hid_leds();
        if ((now & L[i].bit) == (g_saved_leds & L[i].bit)) continue;
        usb_hid_tap(0, L[i].key);
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    ilog_note("  RESTORE_HOST_KEYBOARD_LOCK_STATE -> 0x%02X\n", usb_hid_leds());
}

/* Sleep, but keep listening for the stop button.
 *
 * The whole safety story of this device is that the operator can always halt an
 * injection. That was only true between actions: a payload containing
 * "DELAY 20000" ignored the button for twenty seconds, because the abort flag
 * was read at the top of the action loop and nowhere else. A stop control that
 * works "eventually" is not a stop control. Sleep in short slices instead and
 * return the instant we are told to stop. */
static void sleep_abortable(uint32_t ms, const payload_ctx_t *ctx)
{
    const uint32_t SLICE = 25;
    while (ms) {
        if (ctx->abort && *ctx->abort) return;
        uint32_t chunk = ms < SLICE ? ms : SLICE;
        vTaskDelay(pdMS_TO_TICKS(chunk));
        ms -= chunk;
    }
}

static int g_ilog_idx;    /* keystroke counter for the injection log */
static int g_ilog_line;   /* payload line currently being played          */

static void play_actions(const ducky_action_t *a, int n, uint32_t default_delay,
                         const payload_ctx_t *ctx)
{
    uint8_t held = 0;       /* modifiers held across keys (Unicode sequences) */
    uint8_t held_key = 0;   /* and a normal key, for "HOLD SPACE"             */
    for (int i = 0; i < n; i++) {
        if (ctx->abort && *ctx->abort) break;
        const ducky_action_t *a2 = &a[i];
        if (a2->kind == DUCKY_DELAY)   { sleep_abortable(a2->delay_ms, ctx); continue; }
        if (a2->kind == DUCKY_HOLD) {
            /* An Alt-held run on Windows is the Unicode keypad sequence. */
            if (a2->mods & HID_MOD_LALT) ensure_numlock(ctx);
            held |= a2->mods;
            if (a2->key) held_key = a2->key;
            if (!ctx->dry_run) usb_hid_hold_key(held, held_key); else vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if (a2->kind == DUCKY_RELEASE) { held = 0; held_key = 0; if (!ctx->dry_run) usb_hid_release(); else vTaskDelay(pdMS_TO_TICKS(2)); continue; }
        if (a2->kind == DUCKY_WAIT) {
            /* Block until the host reports the lock state we are waiting for.
             * The LED state arrives on the HID OUT endpoint, so this is the
             * operating system talking back to us - a payload can genuinely
             * synchronise on it. Bounded, so a payload cannot hang for ever on
             * a host that never answers. */
            const uint8_t start = usb_hid_leds();
            uint32_t waited = 0;
            /* If the host has never once sent lock-key state, it is not going
             * to start now - macOS does not send it at all. Waiting the full
             * thirty seconds for an answer that cannot come just looks like the
             * device has hung. Give it a moment in case the first report is
             * merely late, then move on and say so. */
            const uint32_t limit = usb_hid_saw_led_report() ? 30000 : 1500;
            while (waited < limit && !(ctx->abort && *ctx->abort)) {
                uint8_t now = usb_hid_leds();
                bool bit = (now & a2->wait_mask) != 0;
                if (a2->wait_want == 1 && bit) break;
                if (a2->wait_want == 0 && !bit) break;
                if (a2->wait_want == 2 && ((now ^ start) & a2->wait_mask)) break;
                vTaskDelay(pdMS_TO_TICKS(20));
                waited += 20;
            }
            ilog_note("  WAIT_FOR mask=0x%02X want=%u -> %s after %lums%s\n",
                      a2->wait_mask, a2->wait_want,
                      waited >= limit ? "TIMED OUT" : "satisfied", (unsigned long)waited,
                      usb_hid_saw_led_report() ? ""
                        : " (this host has never sent lock-key state)");
            continue;
        }
        if (a2->kind == DUCKY_SPECIAL) {
            /* Device-level commands. In a dry run they are announced and not
             * performed - the whole point of a dry run is that nothing about
             * the machine changes. */
            if (ctx->dry_run) { ilog_note("  [dry] special %u\n", a2->special); continue; }
            switch (a2->special) {
                case DSP_ATTACKMODE_STORAGE:
                    /* Hand the shared partition to the host. The firmware stops
                     * touching the card for as long as this lasts. */
                    if (usb_msc_expose(true))
                        ilog_note("  ATTACKMODE STORAGE: partition %d (%lu MB) exposed\n",
                                  usb_msc_partition(), (unsigned long)usb_msc_size_mb());
                    else
                        ilog_note("  ATTACKMODE STORAGE: no shareable partition on the card\n");
                    break;
                case DSP_ATTACKMODE_HID:
                case DSP_ATTACKMODE_OFF:
                    usb_msc_expose(false);       /* take the card back */
                    ilog_note("  ATTACKMODE: storage returned to the device\n");
                    break;
                case DSP_SAVE_LOCKS:
                    g_saved_leds = usb_hid_leds();
                    g_saved_locks = true;
                    ilog_note("  SAVE_HOST_KEYBOARD_LOCK_STATE: 0x%02X\n", g_saved_leds);
                    break;
                case DSP_RESTORE_LOCKS:
                    if (g_saved_locks) restore_locks(ctx);
                    break;
                case DSP_EXFIL:
                    exfil_write(a2->text);
                    break;
                case DSP_LED_R: case DSP_LED_G: case DSP_LED_OFF:
                    g_payload_led = (a2->special == DSP_LED_OFF) ? 0
                                  : (a2->special == DSP_LED_R ? 1 : 2);
                    break;
                case DSP_WAIT_BUTTON:
                    ilog_note("  WAIT_FOR_BUTTON_PRESS\n");
                    g_wait_button = true;
                    while (g_wait_button && !(ctx->abort && *ctx->abort))
                        vTaskDelay(pdMS_TO_TICKS(20));
                    /* Aborting out of the wait left the flag set, and the UI
                     * swallows one button event while it is - so the first
                     * press after a cancelled run did nothing at all. */
                    g_wait_button = false;
                    break;
                default: break;                  /* accepted, does nothing here */
            }
            continue;
        }
        if (ctx->dry_run)              { vTaskDelay(pdMS_TO_TICKS(2)); continue; }  /* preview */
        if (a2->kind == DUCKY_MOUSE)        usb_hid_mouse(a2->buttons, a2->mx, a2->my, a2->wheel);
        else if (a2->kind == DUCKY_CONSUMER) usb_hid_consumer(a2->consumer);
        else {  /* DUCKY_KEY - compensate for the host's Caps Lock state */
            uint8_t mods = ducky_apply_caps(a2->key, (uint8_t)(held | a2->mods),
                                            usb_hid_leds());
            usb_hid_key(mods, held, a2->key);
            ilog_key(g_ilog_idx++, g_ilog_line, a2->key, mods);
        }
    }
    if ((held || held_key) && !ctx->dry_run) usb_hid_release();  /* never leave a key stuck */
    if (default_delay) sleep_abortable(default_delay, ctx);
}

/* How many lines will actually be EXECUTED?
 *
 * payload_count_lines() counts lines in the file, which is not the same thing
 * at all: comments, blanks and control-flow lines never type anything, while a
 * WHILE loop executes its body many times. Using the file's line count as the
 * denominator made the on-screen progress wrong in both directions - it could
 * sit still while a loop ran, and it could pass the end. Running the
 * interpreter once WITHOUT typing gives the true number. It is cheap: no USB,
 * no delays, and the same step limit that bounds a runaway payload. */
static int count_exec_lines(const char *text)
{
    dscript_t *probe = dscript_shared();
    if (!probe) return payload_count_lines(text);   /* fall back to raw lines */          /* static: 4 KB is too much for the stack */
    if (!dscript_init(probe, text)) return 0;
    int n = 0;
    while (dscript_next(probe) != NULL) n++;
    return n;
}

int payload_run(const char *text, const payload_ctx_t *ctx)
{
    /* 192 actions x 16 bytes = 3 KB. On the payload task's stack that left
     * almost nothing for the line buffers and the parser's own frames, so a
     * payload typed its first character and then overflowed the stack and
     * panicked - taking the whole device (and the web console) down with it.
     * Static is safe here: the state machine runs exactly one payload at a
     * time, and only ever from the payload task. */
    /* 11.5 KB of actions and 8.7 KB of parser state: external RAM, not the
     * internal pool the radio and USB need. Allocated once, never freed. */
    static ducky_action_t *acts;
    if (!acts) acts = (ducky_action_t *)ducky_hot_alloc(sizeof(ducky_action_t) * 192);
    if (!acts) { payload_set_fail("out of memory for the action buffer"); return 0; }
    /* Start from a known keyboard state.
     *
     * HOLD keeps a key down until RELEASE, and a run that is aborted (or ends
     * mid-HOLD) leaves it down. The next run would then send that key inside
     * every report - the host sees it held, starts auto-repeating it, and the
     * text arrives shuffled. Nothing reset it between runs. */
    if (!ctx->dry_run) usb_hid_release();
    s_fail[0] = 0;
    g_ilog_idx = 0; g_ilog_line = 0;
    uint32_t drops_before = usb_hid_drops();
    ilog_open(ctx->name ? ctx->name : "?", ctx);

    /* Two-stage readiness. Enumeration only proves the USB link exists; the
     * lock-key echo proves the operating system is actually consuming
     * keystrokes, which is the thing that matters and the thing that is not
     * true yet on a login screen or a machine still loading a driver. */
    if (!ctx->dry_run) {
        if (!usb_hid_wait_mounted(4000)) {
            ESP_LOGW(TAG, "host has not enumerated us - keystrokes may be lost");
            ilog_note("  ! host had not enumerated the keyboard when the run began\n");
        } else if (usb_hid_wait_host_ready(600)) {
            ilog_note("  host acknowledged the lock-key handshake: input stack is live\n");
        } else {
            /* No handshake available. Settle for a moment so the first
             * keystroke does not race the host finishing its enumeration -
             * this is the whole benefit the handshake used to provide. */
            ilog_note("  no lock-key channel on this host; settling instead\n");
            vTaskDelay(pdMS_TO_TICKS(400));
        }
    }
    ilog_note("  leds at start: 0x%02X (num=%d caps=%d)\n", usb_hid_leds(),
              (usb_hid_leds() & 0x01) ? 1 : 0, (usb_hid_leds() & 0x02) ? 1 : 0);

    /* static: ducky_state_t now carries an 8 KB scratch buffer, and one
     * payload runs at a time. */
    static ducky_state_t *stp;
    if (!stp) stp = (ducky_state_t *)ducky_hot_alloc(sizeof(*stp));
    if (!stp) { payload_set_fail("out of memory for the parser"); return 0; }
    ducky_state_t *st_p = stp; ducky_state_init(st_p);
#define st (*st_p)
    st.rng_state = esp_random();      /* RANDOM_* differs every run */
    st.layout = ctx->layout;
    st.target_os = ctx->os;
    st.default_delay_ms = ctx->default_delay;
    int total = count_exec_lines(text), cur = 0;

    /* The interpreter decides WHICH line runs next: it consumes VAR/IF/WHILE/
     * FUNCTION itself and hands back only the lines that type something, with
     * $variables already substituted. Everything below is unchanged - the
     * player never learned the language. */
    /* The same shared instance: count_exec_lines() has finished with it by
     * the time playback starts. */
    dscript_t *dsp = dscript_shared();
    if (!dsp) {
        ESP_LOGE(TAG, "no memory for the script interpreter");
        payload_set_fail("out of memory for the interpreter");
        return 0;
    }
    dscript_t *ds = dsp;
    if (!dscript_init(ds, text)) {
        /* The screen used to fall back to "PAYLOAD PRODUCED NO KEYSTROKES"
         * here, while the parser knew the exact line and reason. Say the
         * useful thing: the operator is standing at the device. */
        char why[64];
        snprintf(why, sizeof(why), "LINE %u: %s",
                 dscript_error_line(ds), dscript_error(ds) ? dscript_error(ds) : "rejected");
        payload_set_fail(why);
        ESP_LOGE(TAG, "payload rejected: %s (line %u)",
                 dscript_error(ds), dscript_error_line(ds));
        ilog_note("  ! payload rejected: %s (line %u)\n",
                  dscript_error(ds), dscript_error_line(ds));
        ilog_close(0, drops_before);
        return 0;
    }

    const char *line;
    for (;;) {
        /* Feed the host's live state in before EVERY step.
         *
         * Nothing called this at all, so $_OS was always 0 (Windows) whatever
         * the target really was, and every lock-key variable read 0 for ever -
         * IF ($_OS == MAC) could not be true on a Mac, and a payload that waits
         * for CAPS LOCK and then reports it printed a stale zero. It has to be
         * refreshed per step rather than set once: dscript_init() zeroes the
         * struct, and the whole point of these is that they are current. */
        dscript_set_host(ds, (int32_t)ctx->os, usb_hid_leds(), g_wait_button ? 0 : 1);
        /* $_RECEIVED_HOST_LOCK_LED_REPLY: whether the host has EVER sent lock-key
         * state, not whether a light happens to be on. macOS never sends it, so a
         * payload can test this and take the other branch instead of trusting a
         * caps-lock reading that cannot be true. */
        dscript_set_host_usb(ds, usb_hid_mounted() ? 1 : 0,
                             usb_hid_saw_led_report() ? 1 : 0);
        line = dscript_next(ds);
        if (line == NULL) break;
        if (ctx->abort && *ctx->abort) break;
        cur++;
        if (ctx->progress) ctx->progress(cur, total, ctx->user);

        g_ilog_line = cur;
        int n = ducky_parse_line(&st, line, acts, (int)(192));
        ilog_note("  line %d: \"%s\" -> %d action(s)\n", cur, line, n);

        if (st.repeat > 0) {
            char saved[512];
            strncpy(saved, st.last_cmd, sizeof(saved) - 1); saved[sizeof(saved) - 1] = 0;
            int reps = st.repeat;
            for (int r = 0; r < reps && !(ctx->abort && *ctx->abort); r++) {
                int m = ducky_parse_line(&st, saved, acts, (int)(192));
                play_actions(acts, m, st.default_delay_ms, ctx);
                /* A long REPEAT froze the screen on one line number, which
                 * reads exactly like a hung device. The interpreter counts
                 * each repetition, so report them. */
                cur++;
                if (ctx->progress) ctx->progress(cur, total, ctx->user);
            }
            cur--;                     /* the line itself was already counted */
        } else {
            /* A STRING longer than the action buffer arrives in pieces. The
             * default delay belongs at the END of the line, so it is applied
             * only by the chunk that finishes it - not sprinkled through the
             * middle of one long line of text. */
            play_actions(acts, n, st.pending ? 0 : st.default_delay_ms, ctx);
            while (st.pending && !(ctx->abort && *ctx->abort)) {
                int m = ducky_continue(&st, acts, (int)(192));
                if (m <= 0) break;
                play_actions(acts, m, st.pending ? 0 : st.default_delay_ms, ctx);
            }
        }
    }
    if (dscript_error(ds)) {
        if (cur == 0) {                       /* stopped before typing anything */
            char why[64];
            snprintf(why, sizeof(why), "LINE %u: %s",
                     dscript_error_line(ds), dscript_error(ds));
            payload_set_fail(why);
        }
        ESP_LOGW(TAG, "payload stopped: %s (line %u)",
                 dscript_error(ds), dscript_error_line(ds));
        ilog_note("  ! stopped: %s (line %u)\n",
                  dscript_error(ds), dscript_error_line(ds));
    }

#undef st
    ESP_LOGI(TAG, "payload %s (%d lines)%s", (ctx->abort && *ctx->abort) ? "ABORTED" : "finished", cur, ctx->dry_run ? " [dry-run]" : "");
    ilog_close(g_ilog_idx, drops_before);
    return cur;
}
