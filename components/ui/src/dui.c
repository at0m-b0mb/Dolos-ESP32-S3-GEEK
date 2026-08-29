#include "dui.h"
#include <string.h>
#include <stdio.h>

#define W 240
#define H 135

#define DU_BG      cv_rgb(10, 8, 14)
#define DU_PANEL   cv_rgb(26, 16, 24)
#define DU_INK     cv_rgb(236, 228, 240)
#define DU_DIM     cv_rgb(140, 120, 150)
#define DU_ACCENT  cv_rgb(230, 70, 90)
#define DU_SAFE    cv_rgb(60, 200, 130)
#define DU_ARMED   cv_rgb(240, 180, 60)
#define DU_FIRE    cv_rgb(240, 70, 70)
#define DU_DONE    cv_rgb(90, 160, 255)

static uint16_t mode_color(dui_mode_t m)
{
    switch (m) {
        case DUI_SAFE:      return DU_SAFE;
        case DUI_PINENTRY:  return DU_ARMED;
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
        case DUI_PINENTRY:  return "PIN";
        case DUI_ARMED:     return "ARMED";
        case DUI_COUNTDOWN: return "FIRING";
        case DUI_RUNNING:   return "RUNNING";
        default:            return "SENT";
    }
}

static void draw_kbd(canvas_t *cv, int x, int y, uint16_t c)
{
    cv_rect(cv, x, y, 18, 11, c);
    for (int r = 0; r < 2; r++)
        for (int k = 0; k < 5; k++)
            cv_pixel(cv, x + 3 + k * 3, y + 3 + r * 3, c);
    cv_hline(cv, x + 5, y + 8, 8, c);
}

static void draw_header(canvas_t *cv, const dui_state_t *st)
{
    cv_fill_rect(cv, 0, 0, W, 15, DU_PANEL);
    draw_kbd(cv, 4, 2, DU_ACCENT);
    cv_text(cv, 27, 1, "DOLOS", DU_INK, -1, 2);
    if (st->wifi_on) cv_fill_circle(cv, 80, 7, 3, DU_SAFE);   /* wireless console live */
    if (st->remote_fire_enabled) { cv_fill_rect(cv, 90, 3, 24, 10, DU_FIRE); cv_text(cv, 93, 4, "RF", DU_BG, -1, 1); }
    else if (st->dry_run)        { cv_fill_rect(cv, 90, 3, 34, 10, DU_ARMED); cv_text(cv, 93, 4, "DRY", DU_BG, -1, 1); }
    const char *u = st->usb_mounted ? "USB LINK" : "NO HOST";
    uint16_t uc = st->usb_mounted ? DU_SAFE : DU_DIM;
    cv_fill_circle(cv, W - cv_text_width(u, 1) - 9, 7, 3, uc);
    cv_text(cv, W - cv_text_width(u, 1) - 3, 4, u, uc, -1, 1);
    cv_hline(cv, 0, 15, W, st->remote_fire_enabled ? DU_FIRE : DU_ACCENT);
}

/* persistent, non-covert banner whenever the admin has enabled remote fire */
static void draw_rf_banner(canvas_t *cv)
{
    cv_fill_rect(cv, 0, H - 25, W, 11, DU_FIRE);
    cv_text_center(cv, W / 2, H - 24, "REMOTE FIRE ARMED", DU_BG, -1, 1);
}

static void draw_footer(canvas_t *cv, const dui_state_t *st)
{
    cv_hline(cv, 0, H - 12, W, cv_rgb(40, 26, 38));
    char buf[40];
    snprintf(buf, sizeof(buf), "%s / %s", st->layout ? st->layout : "US",
             st->speed ? st->speed : "BAL");
    cv_text(cv, 4, H - 10, buf, DU_DIM, -1, 1);
    /* keyboard-LED return channel: lit letters = bit set on the host */
    int lx = 96;
    cv_text(cv, lx,      H - 10, "C", (st->leds & 0x02) ? DU_SAFE : cv_rgb(60,50,58), -1, 1);
    cv_text(cv, lx + 8,  H - 10, "N", (st->leds & 0x01) ? DU_SAFE : cv_rgb(60,50,58), -1, 1);
    cv_text(cv, lx + 16, H - 10, "S", (st->leds & 0x04) ? DU_SAFE : cv_rgb(60,50,58), -1, 1);
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
    cv_text_center(cv, W / 2, 24, mode_word(st->mode), mc, -1, 3);

    switch (st->mode) {
        case DUI_SAFE: {
            char pl[40];
            if (st->payload_count > 1)
                snprintf(pl, sizeof(pl), "%d/%d  %s", st->payload_idx, st->payload_count,
                         st->payload_name ? st->payload_name : "-");
            else
                snprintf(pl, sizeof(pl), "%s", st->payload_name ? st->payload_name : "demo");
            cv_text_center(cv, W / 2, 54, pl, DU_INK, -1, 1);
            cv_text_center(cv, W / 2, 70,
                st->payload_count > 1 ? "TAP = NEXT   HOLD = ARM" : "HOLD BOOT TO ARM", DU_DIM, -1, 1);
            if (st->wifi_on) {
                char ap[40]; snprintf(ap, sizeof(ap), "AP %s", st->wifi_ssid ? st->wifi_ssid : "-");
                cv_text_center(cv, W / 2, 88, ap, DU_SAFE, -1, 1);
                if (st->admin_pw) {
                    char pw[32]; snprintf(pw, sizeof(pw), "PW %s", st->admin_pw);
                    cv_text_center(cv, W / 2, 100, pw, DU_ARMED, -1, 1);
                }
            } else {
                cv_text_center(cv, W / 2, 90, "DEVICE WILL NOT TYPE", cv_rgb(90,74,86), -1, 1);
            }
            break;
        }
        case DUI_PINENTRY: {
            cv_text_center(cv, W / 2, 52, "ENTER PIN", DU_INK, -1, 1);
            /* committed digits as dots, current digit as a number */
            int total = st->pin_len > 0 ? st->pin_len : 1;
            int cx = W / 2 - (total * 12) / 2;
            for (int i = 0; i < total; i++) {
                if (i < st->pin_pos) cv_fill_circle(cv, cx + i * 12 + 4, 70, 4, DU_SAFE);
                else                 cv_circle(cv, cx + i * 12 + 4, 70, 4, DU_DIM);
            }
            char d[4]; snprintf(d, sizeof(d), "%d", st->pin_cur);
            cv_text_center(cv, W / 2, 82, d, DU_ARMED, -1, 2);
            cv_text_center(cv, W / 2, 104, "TAP = DIGIT   HOLD = OK", DU_DIM, -1, 1);
            break;
        }
        case DUI_ARMED:
            cv_text_center(cv, W / 2, 56, "HOLD BOOT TO FIRE", DU_INK, -1, 1);
            cv_text_center(cv, W / 2, 74, "TAP TO CANCEL", DU_DIM, -1, 1);
            break;
        case DUI_COUNTDOWN: {
            char n[4]; snprintf(n, sizeof(n), "%d", st->countdown);
            cv_text_center(cv, W / 2, 50, n, DU_FIRE, -1, 5);
            cv_text_center(cv, W / 2, 100, "TAP TO ABORT", DU_DIM, -1, 1);
            break;
        }
        case DUI_RUNNING:
            progress_bar(cv, 24, 60, W - 48, 9, st->cur_line, st->total_lines, st->dry_run ? DU_ARMED : DU_FIRE);
            {
                char b[32]; snprintf(b, sizeof(b), "%sLINE %d / %d",
                                     st->dry_run ? "DRY  " : "", st->cur_line, st->total_lines);
                cv_text_center(cv, W / 2, 76, b, DU_INK, -1, 1);
            }
            cv_text_center(cv, W / 2, 92, "TAP TO ABORT", DU_DIM, -1, 1);
            break;
        case DUI_DONE:
            cv_text_center(cv, W / 2, 60, st->dry_run ? "DRY-RUN COMPLETE" : "PAYLOAD SENT", DU_INK, -1, 1);
            cv_text_center(cv, W / 2, 78, "TAP TO RETURN TO SAFE", DU_DIM, -1, 1);
            break;
    }
    if (st->remote_fire_enabled) draw_rf_banner(cv);
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
