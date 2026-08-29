#include "dui.h"
#include <string.h>
#include <stdio.h>

#define W 240
#define H 135

/* Dolos palette: red-team accent, safety-forward state colors. */
#define DU_BG      cv_rgb(10, 8, 14)
#define DU_PANEL   cv_rgb(26, 16, 24)
#define DU_INK     cv_rgb(236, 228, 240)
#define DU_DIM     cv_rgb(140, 120, 150)
#define DU_ACCENT  cv_rgb(230, 70, 90)     /* Dolos crimson            */
#define DU_SAFE    cv_rgb(60, 200, 130)    /* green                    */
#define DU_ARMED   cv_rgb(240, 180, 60)    /* amber                    */
#define DU_FIRE    cv_rgb(240, 70, 70)     /* red                      */
#define DU_DONE    cv_rgb(90, 160, 255)    /* blue                     */

static uint16_t mode_color(dui_mode_t m)
{
    switch (m) {
        case DUI_SAFE:      return DU_SAFE;
        case DUI_ARMED:     return DU_ARMED;
        case DUI_COUNTDOWN: return DU_FIRE;
        case DUI_RUNNING:   return DU_FIRE;
        default:            return DU_DONE;
    }
}
static const char *mode_word(dui_mode_t m)
{
    switch (m) {
        case DUI_SAFE:      return "SAFE";
        case DUI_ARMED:     return "ARMED";
        case DUI_COUNTDOWN: return "FIRING";
        case DUI_RUNNING:   return "RUNNING";
        default:            return "SENT";
    }
}

/* a little keyboard glyph, to say "this is a keyboard" */
static void draw_kbd(canvas_t *cv, int x, int y, uint16_t c)
{
    cv_rect(cv, x, y, 18, 11, c);
    for (int r = 0; r < 2; r++)
        for (int k = 0; k < 5; k++)
            cv_pixel(cv, x + 3 + k * 3, y + 3 + r * 3, c);
    cv_hline(cv, x + 5, y + 8, 8, c);           /* space bar */
}

static void draw_header(canvas_t *cv, const dui_state_t *st)
{
    cv_fill_rect(cv, 0, 0, W, 15, DU_PANEL);
    draw_kbd(cv, 4, 2, DU_ACCENT);
    cv_text(cv, 27, 1, "DOLOS", DU_INK, -1, 2);
    /* USB mount indicator, right */
    const char *u = st->usb_mounted ? "USB LINK" : "NO HOST";
    uint16_t uc = st->usb_mounted ? DU_SAFE : DU_DIM;
    cv_fill_circle(cv, W - cv_text_width(u, 1) - 9, 7, 3, uc);
    cv_text(cv, W - cv_text_width(u, 1) - 3, 4, u, uc, -1, 1);
    cv_hline(cv, 0, 15, W, DU_ACCENT);
}

static void draw_footer(canvas_t *cv, const dui_state_t *st)
{
    cv_hline(cv, 0, H - 12, W, cv_rgb(40, 26, 38));
    char buf[40];
    snprintf(buf, sizeof(buf), "%s  %d LN",
             st->payload_name ? st->payload_name : "-", st->total_lines);
    cv_text(cv, 4, H - 10, buf, DU_DIM, -1, 1);
    cv_text(cv, W - cv_text_width("LAB USE ONLY", 1) - 3, H - 10, "LAB USE ONLY", DU_ACCENT, -1, 1);
}

static void progress_bar(canvas_t *cv, int x, int y, int w, int h, int cur, int total, uint16_t c)
{
    cv_rect(cv, x, y, w, h, DU_DIM);
    if (total > 0) {
        int fw = (w - 2) * cur / total;
        if (fw < 0) fw = 0;
        if (fw > w - 2) fw = w - 2;
        if (fw > 0) cv_fill_rect(cv, x + 1, y + 1, fw, h - 2, c);
    }
}

void dui_render(canvas_t *cv, const dui_state_t *st)
{
    cv_clear(cv, DU_BG);
    uint16_t mc = mode_color(st->mode);

    /* big state word, centred */
    cv_text_center(cv, W / 2, 26, mode_word(st->mode), mc, -1, 3);

    switch (st->mode) {
        case DUI_SAFE:
            cv_text_center(cv, W / 2, 58, "DEVICE WILL NOT TYPE", DU_DIM, -1, 1);
            cv_text_center(cv, W / 2, 78, "HOLD  BOOT  TO  ARM", DU_INK, -1, 1);
            break;
        case DUI_ARMED:
            cv_text_center(cv, W / 2, 58, "HOLD BOOT TO FIRE", DU_INK, -1, 1);
            cv_text_center(cv, W / 2, 74, "TAP TO CANCEL", DU_DIM, -1, 1);
            break;
        case DUI_COUNTDOWN: {
            char n[4]; snprintf(n, sizeof(n), "%d", st->countdown);
            cv_text_center(cv, W / 2, 52, n, DU_FIRE, -1, 5);
            cv_text_center(cv, W / 2, 98, "TAP TO ABORT", DU_DIM, -1, 1);
            break;
        }
        case DUI_RUNNING:
            progress_bar(cv, 24, 62, W - 48, 9, st->cur_line, st->total_lines, DU_FIRE);
            {
                char b[28]; snprintf(b, sizeof(b), "LINE %d / %d", st->cur_line, st->total_lines);
                cv_text_center(cv, W / 2, 78, b, DU_INK, -1, 1);
            }
            cv_text_center(cv, W / 2, 94, "TAP TO ABORT", DU_DIM, -1, 1);
            break;
        case DUI_DONE:
            cv_text_center(cv, W / 2, 62, "PAYLOAD SENT", DU_INK, -1, 1);
            cv_text_center(cv, W / 2, 80, "TAP TO RETURN TO SAFE", DU_DIM, -1, 1);
            break;
    }
    draw_header(cv, st);
    draw_footer(cv, st);
}

void dui_render_splash(canvas_t *cv)
{
    cv_clear(cv, DU_BG);
    draw_kbd(cv, W / 2 - 46, 44, DU_ACCENT);
    cv_text(cv, W / 2 - 22, 40, "DOLOS", DU_INK, -1, 3);
    cv_text_center(cv, W / 2, 74, "USB-HID PAYLOAD RUNNER", DU_DIM, -1, 1);
    cv_text_center(cv, W / 2, 92, "AUTHORIZED LAB USE ONLY", DU_ACCENT, -1, 1);
}
