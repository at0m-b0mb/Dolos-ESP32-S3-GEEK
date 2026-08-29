#include "payload.h"
#include "usb_hid.h"
#include "ducky.h"
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "payload";

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

static void play_actions(const ducky_action_t *a, int n, uint32_t default_delay,
                         const payload_ctx_t *ctx)
{
    uint8_t held = 0;   /* modifiers held across keys (Unicode sequences) */
    for (int i = 0; i < n; i++) {
        if (ctx->abort && *ctx->abort) break;
        const ducky_action_t *a2 = &a[i];
        if (a2->kind == DUCKY_DELAY)   { vTaskDelay(pdMS_TO_TICKS(a2->delay_ms)); continue; }
        if (a2->kind == DUCKY_HOLD)    { held |= a2->mods; if (!ctx->dry_run) usb_hid_hold(held); else vTaskDelay(pdMS_TO_TICKS(2)); continue; }
        if (a2->kind == DUCKY_RELEASE) { held = 0; if (!ctx->dry_run) usb_hid_release(); else vTaskDelay(pdMS_TO_TICKS(2)); continue; }
        if (ctx->dry_run)              { vTaskDelay(pdMS_TO_TICKS(2)); continue; }  /* preview */
        if (a2->kind == DUCKY_MOUSE)        usb_hid_mouse(a2->buttons, a2->mx, a2->my, a2->wheel);
        else if (a2->kind == DUCKY_CONSUMER) usb_hid_consumer(a2->consumer);
        else /* DUCKY_KEY */                usb_hid_key((uint8_t)(held | a2->mods), held, a2->key);
    }
    if (held && !ctx->dry_run) usb_hid_release();   /* never leave modifiers stuck */
    if (default_delay) vTaskDelay(pdMS_TO_TICKS(default_delay));
}

int payload_run(const char *text, const payload_ctx_t *ctx)
{
    ducky_action_t acts[192];
    ducky_state_t st; ducky_state_init(&st);
    st.layout = ctx->layout;
    st.target_os = ctx->os;
    st.default_delay_ms = ctx->default_delay;
    int total = payload_count_lines(text), cur = 0;

    const char *p = text;
    char line[224];
    while (*p) {
        if (ctx->abort && *ctx->abort) break;
        size_t l = 0;
        while (*p && *p != '\n' && l < sizeof(line) - 1) line[l++] = *p++;
        line[l] = 0;
        if (*p == '\n') p++;
        cur++;
        if (ctx->progress) ctx->progress(cur, total, ctx->user);

        int n = ducky_parse_line(&st, line, acts, (int)(sizeof(acts) / sizeof(acts[0])));
        if (st.repeat > 0) {
            char saved[160]; strncpy(saved, st.last_cmd, sizeof(saved) - 1); saved[sizeof(saved)-1]=0;
            int reps = st.repeat;
            for (int r = 0; r < reps && !(ctx->abort && *ctx->abort); r++) {
                int m = ducky_parse_line(&st, saved, acts, (int)(sizeof(acts)/sizeof(acts[0])));
                play_actions(acts, m, st.default_delay_ms, ctx);
            }
        } else {
            play_actions(acts, n, st.default_delay_ms, ctx);
        }
    }
    ESP_LOGI(TAG, "payload %s (%d lines)%s", (ctx->abort && *ctx->abort) ? "ABORTED" : "finished", cur, ctx->dry_run ? " [dry-run]" : "");
    return cur;
}
