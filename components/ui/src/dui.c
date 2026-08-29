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

/* Never leaves the screen. */
#define WARN_TAG   "AUTHORIZED USE ONLY"

static uint16_t mode_color(dui_mode_t m)
{
    switch (m) {
        case DUI_SAFE:      return DU_SAFE;
        case DUI_PINENTRY:  return DU_ARMED;
        case DUI_ARMED:     return DU_ARMED;
        case DUI_COUNTDOWN: return DU_FIRE;
        case DUI_RUNNING:   return DU_FIRE;
        case DUI_MENU:
        case DUI_INFO:      return DU_DONE;
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
        case DUI_MENU:      return "SETTINGS";
        case DUI_INFO:      return "CONSOLE";
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
    /* One badge slot, most important state wins: a live remote-fire beats a
     * dry run, and either beats telling you the buttons are locked. */
    if (st->remote_fire_enabled)      { cv_fill_rect(cv, 90, 3, 24, 10, DU_FIRE);  cv_text(cv, 93, 4, "RF",   DU_BG, -1, 1); }
    else if (st->dry_run)             { cv_fill_rect(cv, 90, 3, 28, 10, DU_ARMED); cv_text(cv, 93, 4, "DRY",  DU_BG, -1, 1); }
    else if (st->ui_lock == UI_LOCK_FULL) { cv_fill_rect(cv, 90, 3, 34, 10, DU_DIM); cv_text(cv, 93, 4, "LOCK", DU_BG, -1, 1); }
    else if (st->ui_lock == UI_LOCK_MENU) { cv_rect(cv, 90, 3, 34, 10, DU_DIM);      cv_text(cv, 93, 4, "LOCK", DU_DIM, -1, 1); }
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
    /* The standing reminder. "AUTHORIZED USE ONLY" rather than "lab": this ships
     * into real engagements, where the thing that makes use legitimate is
     * written authorization, not the room you are standing in. 19 chars = 114 px
     * right-aligned at x=123, clearing the LED indicators that end at 118. */
    cv_text(cv, W - cv_text_width(WARN_TAG, 1) - 3, H - 10, WARN_TAG, DU_ACCENT, -1, 1);
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

/* The settings screen. One button, so the whole list is on screen at once and
 * the highlighted row IS the cursor - there is no scrolling to get lost in. */
static void draw_menu(canvas_t *cv, const dui_state_t *st)
{
    /* The list outgrew the panel once it passed eight entries, so it scrolls:
     * a fixed window of rows follows the selection, with arrows showing that
     * there is more above or below. The highlighted row is still the cursor. */
    const int y0 = 19, rh = 12;
    const int visible = (H - 12 - y0) / rh;          /* rows that fit above the footer */
    int first = st->menu_sel - visible / 2;
    if (first > MENU__COUNT - visible) first = MENU__COUNT - visible;
    if (first < 0) first = 0;

    for (int row = 0; row < visible && (first + row) < MENU__COUNT; row++) {
        int item = first + row;
        int y = y0 + row * rh;
        bool sel = (item == st->menu_sel);
        if (sel) cv_fill_rect(cv, 2, y - 1, W - 4, rh - 1, DU_ACCENT);
        uint16_t fg = sel ? DU_BG : DU_INK;
        cv_text(cv, 6, y, menu_label((menu_item_t)item), fg, -1, 1);
        if (st->cfg) {
            char v[24];
            menu_value(st->cfg, (menu_item_t)item, v, sizeof(v));
            if (v[0]) cv_text(cv, W - cv_text_width(v, 1) - 12, y, v,
                              sel ? DU_BG : DU_SAFE, -1, 1);
        }
    }
    /* more-above / more-below markers, in the right margin */
    if (first > 0)                        cv_text(cv, W - 8, y0, "^", DU_DIM, -1, 1);
    if (first + visible < MENU__COUNT)    cv_text(cv, W - 8, y0 + (visible - 1) * rh, "v", DU_DIM, -1, 1);
}

/* Draw text clipped to `maxw` pixels: at scale 1, cutting off any tail that
 * would not fit rather than letting it run past the edge. */
static void draw_fit_scale1(canvas_t *cv, int x, int y, const char *s,
                            uint16_t colour, int maxw)
{
    char buf[64];
    int max_chars = maxw / 6;
    if (max_chars <= 0) return;
    if (max_chars > (int)sizeof(buf) - 1) max_chars = (int)sizeof(buf) - 1;
    snprintf(buf, (size_t)max_chars + 1, "%s", s ? s : "-");
    cv_text(cv, x, y, buf, colour, -1, 1);
}

/* Same, but preferring double size when the whole string fits at that size. */
static void draw_fit(canvas_t *cv, int x, int y, const char *s,
                     uint16_t colour, int maxw)
{
    const char *t = s ? s : "-";
    if (cv_text_width(t, 2) <= maxw) { cv_text(cv, x, y, t, colour, -1, 2); return; }
    draw_fit_scale1(cv, x, y, t, colour, maxw);
}

/* The console join screen.
 *
 * Reading a 10-character key off a 1.14" screen and typing it into a phone is
 * miserable, so the credentials are encoded as a standard Wi-Fi QR (the same
 * "WIFI:" URI Android and iOS cameras understand) and the text beside it is
 * drawn at double size to be legible at arm's length. */
static void draw_info(canvas_t *cv, const dui_state_t *st)
{
    if (!st->wifi_on) {
        cv_text_center(cv, W / 2, 56, "WIFI CONSOLE IS OFF", DU_DIM, -1, 1);
        cv_text_center(cv, W / 2, 74, "ENABLE IT IN SETTINGS", DU_DIM, -1, 1);
        return;
    }

    /* WIFI:T:WPA;S:<ssid>;P:<key>;; - the join URI phone cameras recognise */
    char uri[128];
    snprintf(uri, sizeof(uri), "WIFI:T:WPA;S:%s;P:%s;;",
             st->wifi_ssid ? st->wifi_ssid : "", st->wifi_key ? st->wifi_key : "");

    static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
    static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
    bool ok = qrcodegen_encodeText(uri, tmp, qr, qrcodegen_Ecc_LOW,
                                   1, 6, qrcodegen_Mask_AUTO, true);
    int qx = 6, qy = 20;
    if (ok) {
        int n = qrcodegen_getSize(qr);
        int avail = H - 12 - qy - 2;               /* above the footer */
        int scale = avail / (n + 2);               /* +2 = quiet zone  */
        if (scale < 1) scale = 1;
        int side = (n + 2) * scale;
        /* A QR needs a light quiet zone to scan: draw the whole block white. */
        cv_fill_rect(cv, qx, qy, side, side, cv_rgb(255, 255, 255));
        for (int y = 0; y < n; y++)
            for (int x = 0; x < n; x++)
                if (qrcodegen_getModule(qr, x, y))
                    cv_fill_rect(cv, qx + (x + 1) * scale, qy + (y + 1) * scale,
                                 scale, scale, cv_rgb(0, 0, 0));
        qx += side + 6;
    }

    /* Credentials as large as they will go without spilling out of the column
     * beside the QR. Draw them at double size when they fit, single otherwise,
     * and clip to the column either way - a value that runs off the panel is
     * worse than a small one, because you cannot tell it was truncated. */
    int tx = qx, tw = W - tx - 4;
    cv_text(cv, tx, 18, "NETWORK", DU_DIM, -1, 1);
    draw_fit(cv, tx, 27, st->wifi_ssid ? st->wifi_ssid : "-", DU_SAFE, tw);
    cv_text(cv, tx, 47, "KEY", DU_DIM, -1, 1);
    draw_fit(cv, tx, 56, st->wifi_key ? st->wifi_key : "-", DU_INK, tw);
    {
        char who[40];
        snprintf(who, sizeof(who), "LOGIN  %s", st->admin_user ? st->admin_user : "admin");
        draw_fit_scale1(cv, tx, 76, who, DU_DIM, tw);
    }
    draw_fit(cv, tx, 85, st->admin_pw ? st->admin_pw : "-", DU_ARMED, tw);
    draw_fit_scale1(cv, tx, 106, "http://192.168.4.1", DU_DIM, tw);
}

void dui_render(canvas_t *cv, const dui_state_t *st)
{
    cv_clear(cv, DU_BG);
    if (st->mode == DUI_MENU) {          /* the list needs the whole screen */
        draw_menu(cv, st);
        draw_header(cv, st);
        draw_footer(cv, st);
        return;
    }
    if (st->mode == DUI_INFO) {
        draw_info(cv, st);
        draw_header(cv, st);
        draw_footer(cv, st);
        return;
    }
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
            if (st->lint_problems > 0) {
                /* A payload that will not do what it says is a hazard on a live
                 * engagement, so the device refuses to arm and says why. */
                char le[40];
                snprintf(le, sizeof(le), "PAYLOAD ERROR  LINE %d", st->lint_line);
                cv_text_center(cv, W / 2, 68, le, DU_FIRE, -1, 1);
                if (st->lint_msg) cv_text_center(cv, W / 2, 80, st->lint_msg, DU_DIM, -1, 1);
                cv_text_center(cv, W / 2, 96, "ARMING BLOCKED", DU_ARMED, -1, 1);
            } else {
                /* The hint must describe what the button will ACTUALLY do at
                 * this lock level, or a locked device just looks broken. */
                const char *hint =
                    (st->ui_lock >= UI_LOCK_FULL || st->payload_count <= 1)
                        ? "HOLD BOOT TO ARM" : "TAP = NEXT   HOLD = ARM";
                cv_text_center(cv, W / 2, 70, hint, DU_DIM, -1, 1);
                if (st->wifi_on) {
                    /* Just the network name here; the key and the console login
                     * live on the CONSOLE screen, drawn large and with a QR. */
                    char l1[44];
                    snprintf(l1, sizeof(l1), "AP  %s", st->wifi_ssid ? st->wifi_ssid : "-");
                    cv_text_center(cv, W / 2, 88, l1, DU_SAFE, -1, 1);
                    if (st->ui_lock == UI_LOCK_OFF)
                        cv_text_center(cv, W / 2, 104, "SETTINGS > CONSOLE INFO",
                                       cv_rgb(90, 74, 86), -1, 1);
                } else if (st->safe_boot) {
                    /* Tell the operator the device chose to come up reduced,
                     * rather than leaving them to wonder where the radio went. */
                    cv_text_center(cv, W / 2, 88, "SAFE BOOT - RADIO OFF", DU_ARMED, -1, 1);
                    cv_text_center(cv, W / 2, 100, "LAST BOOT CRASHED", DU_DIM, -1, 1);
                } else {
                    cv_text_center(cv, W / 2, 90, "DEVICE WILL NOT TYPE", cv_rgb(90,74,86), -1, 1);
                    cv_text_center(cv, W / 2, 108,
                        st->ui_lock == UI_LOCK_OFF ? "DOUBLE-TAP = SETTINGS" : "SETTINGS LOCKED",
                        cv_rgb(90,74,86), -1, 1);
                }
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
        case DUI_MENU:
        case DUI_INFO: break;   /* drawn above; listed so -Wswitch stays happy */
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
    cv_text_center(cv, W / 2, 92, WARN_TAG, DU_ACCENT, -1, 1);
}
