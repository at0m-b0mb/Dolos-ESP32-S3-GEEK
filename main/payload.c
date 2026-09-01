#include "payload.h"
#include "usb_hid.h"
#include "ducky.h"
#include "dscript.h"
#include "hid_keys.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include <stdarg.h>

static const char *TAG = "payload";

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
    fflush(s_ilog);
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

static int g_ilog_idx;    /* keystroke counter for the injection log */
static int g_ilog_line;   /* payload line currently being played          */

static void play_actions(const ducky_action_t *a, int n, uint32_t default_delay,
                         const payload_ctx_t *ctx)
{
    uint8_t held = 0;   /* modifiers held across keys (Unicode sequences) */
    for (int i = 0; i < n; i++) {
        if (ctx->abort && *ctx->abort) break;
        const ducky_action_t *a2 = &a[i];
        if (a2->kind == DUCKY_DELAY)   { vTaskDelay(pdMS_TO_TICKS(a2->delay_ms)); continue; }
        if (a2->kind == DUCKY_HOLD) {
            /* An Alt-held run on Windows is the Unicode keypad sequence. */
            if (a2->mods & HID_MOD_LALT) ensure_numlock(ctx);
            held |= a2->mods;
            if (!ctx->dry_run) usb_hid_hold(held); else vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if (a2->kind == DUCKY_RELEASE) { held = 0; if (!ctx->dry_run) usb_hid_release(); else vTaskDelay(pdMS_TO_TICKS(2)); continue; }
        if (a2->kind == DUCKY_WAIT) {
            /* Block until the host reports the lock state we are waiting for.
             * The LED state arrives on the HID OUT endpoint, so this is the
             * operating system talking back to us - a payload can genuinely
             * synchronise on it. Bounded, so a payload cannot hang for ever on
             * a host that never answers. */
            const uint8_t start = usb_hid_leds();
            uint32_t waited = 0;
            const uint32_t limit = 30000;
            while (waited < limit && !(ctx->abort && *ctx->abort)) {
                uint8_t now = usb_hid_leds();
                bool bit = (now & a2->wait_mask) != 0;
                if (a2->wait_want == 1 && bit) break;
                if (a2->wait_want == 0 && !bit) break;
                if (a2->wait_want == 2 && ((now ^ start) & a2->wait_mask)) break;
                vTaskDelay(pdMS_TO_TICKS(20));
                waited += 20;
            }
            ilog_note("  WAIT_FOR mask=0x%02X want=%u -> %s after %lums\n",
                      a2->wait_mask, a2->wait_want,
                      waited >= limit ? "TIMED OUT" : "satisfied", (unsigned long)waited);
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
    if (held && !ctx->dry_run) usb_hid_release();   /* never leave modifiers stuck */
    if (default_delay) vTaskDelay(pdMS_TO_TICKS(default_delay));
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
    static dscript_t probe;          /* static: 4 KB is too much for the stack */
    if (!dscript_init(&probe, text)) return 0;
    int n = 0;
    while (dscript_next(&probe) != NULL) n++;
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
    static ducky_action_t acts[192];
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
            ilog_note("  ! no lock-key echo; host may not report synchronously\n");
            vTaskDelay(pdMS_TO_TICKS(300));      /* fall back to a fixed pause */
        }
    }
    ilog_note("  leds at start: 0x%02X (num=%d caps=%d)\n", usb_hid_leds(),
              (usb_hid_leds() & 0x01) ? 1 : 0, (usb_hid_leds() & 0x02) ? 1 : 0);

    ducky_state_t st; ducky_state_init(&st);
    st.rng_state = esp_random();      /* RANDOM_* differs every run */
    st.layout = ctx->layout;
    st.target_os = ctx->os;
    st.default_delay_ms = ctx->default_delay;
    int total = count_exec_lines(text), cur = 0;

    /* The interpreter decides WHICH line runs next: it consumes VAR/IF/WHILE/
     * FUNCTION itself and hands back only the lines that type something, with
     * $variables already substituted. Everything below is unchanged - the
     * player never learned the language. */
    static dscript_t ds;
    if (!dscript_init(&ds, text)) {
        ESP_LOGE(TAG, "payload rejected: %s (line %u)",
                 dscript_error(&ds), dscript_error_line(&ds));
        ilog_note("  ! payload rejected: %s (line %u)\n",
                  dscript_error(&ds), dscript_error_line(&ds));
        ilog_close(0, drops_before);
        return 0;
    }

    const char *line;
    while ((line = dscript_next(&ds)) != NULL) {
        if (ctx->abort && *ctx->abort) break;
        cur++;
        if (ctx->progress) ctx->progress(cur, total, ctx->user);

        g_ilog_line = cur;
        int n = ducky_parse_line(&st, line, acts, (int)(sizeof(acts) / sizeof(acts[0])));
        ilog_note("  line %d: \"%s\" -> %d action(s)\n", cur, line, n);

        if (st.repeat > 0) {
            char saved[2048];
            strncpy(saved, st.last_cmd, sizeof(saved) - 1); saved[sizeof(saved) - 1] = 0;
            int reps = st.repeat;
            for (int r = 0; r < reps && !(ctx->abort && *ctx->abort); r++) {
                int m = ducky_parse_line(&st, saved, acts, (int)(sizeof(acts) / sizeof(acts[0])));
                play_actions(acts, m, st.default_delay_ms, ctx);
            }
        } else {
            play_actions(acts, n, st.default_delay_ms, ctx);
        }
    }
    if (dscript_error(&ds)) {
        ESP_LOGW(TAG, "payload stopped: %s (line %u)",
                 dscript_error(&ds), dscript_error_line(&ds));
        ilog_note("  ! stopped: %s (line %u)\n",
                  dscript_error(&ds), dscript_error_line(&ds));
    }

    ESP_LOGI(TAG, "payload %s (%d lines)%s", (ctx->abort && *ctx->abort) ? "ABORTED" : "finished", cur, ctx->dry_run ? " [dry-run]" : "");
    ilog_close(g_ilog_idx, drops_before);
    return cur;
}
