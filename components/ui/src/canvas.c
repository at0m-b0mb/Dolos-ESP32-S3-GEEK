#include "canvas.h"
#include <string.h>

void cv_init(canvas_t *cv, uint16_t *buf, int w, int h)
{
    cv->px = buf; cv->w = w; cv->h = h; cv->oob = 0;
}

void cv_pixel(canvas_t *cv, int x, int y, uint16_t color)
{
    if (x < 0 || y < 0 || x >= cv->w || y >= cv->h) { cv->oob++; return; }
    cv->px[y * cv->w + x] = color;
}

void cv_clear(canvas_t *cv, uint16_t color)
{
    /* Fill one row, then replicate it: memcpy moves words, the scalar loop
     * moved one 16-bit pixel per iteration over all 32,400 of them. */
    uint16_t *px = cv->px;
    for (int x = 0; x < cv->w; x++) px[x] = color;
    for (int y = 1; y < cv->h; y++)
        memcpy(px + (size_t)y * cv->w, px, (size_t)cv->w * sizeof(uint16_t));
}

void cv_hline(canvas_t *cv, int x, int y, int w, uint16_t color)
{
    for (int i = 0; i < w; i++) cv_pixel(cv, x + i, y, color);
}

void cv_vline(canvas_t *cv, int x, int y, int h, uint16_t color)
{
    for (int i = 0; i < h; i++) cv_pixel(cv, x, y + i, color);
}

void cv_fill_rect(canvas_t *cv, int x, int y, int w, int h, uint16_t color)
{
    /* Clip once, then write whole rows. The previous version called cv_pixel
     * per pixel, paying a call + four bounds compares for every one - and text
     * rendering fills a rect per glyph pixel, so this is the hottest path in
     * the UI. Out-of-bounds pixels are still COUNTED (not silently dropped) so
     * the oob guard the UI tests rely on keeps the exact same meaning. */
    if (w <= 0 || h <= 0) return;
    int x0 = x < 0 ? 0 : x,           y0 = y < 0 ? 0 : y;
    int x1 = x + w > cv->w ? cv->w : x + w;
    int y1 = y + h > cv->h ? cv->h : y + h;
    long total = (long)w * h, drawn = 0;
    if (x1 > x0 && y1 > y0) {
        drawn = (long)(x1 - x0) * (y1 - y0);
        for (int j = y0; j < y1; j++) {
            uint16_t *row = cv->px + (size_t)j * cv->w + x0;
            for (int i = x0; i < x1; i++) *row++ = color;
        }
    }
    cv->oob += (uint32_t)(total - drawn);
}

void cv_rect(canvas_t *cv, int x, int y, int w, int h, uint16_t color)
{
    cv_hline(cv, x, y, w, color);
    cv_hline(cv, x, y + h - 1, w, color);
    cv_vline(cv, x, y, h, color);
    cv_vline(cv, x + w - 1, y, h, color);
}

void cv_line(canvas_t *cv, int x0, int y0, int x1, int y1, uint16_t color)
{
    int dx = x1 - x0, dy = y1 - y0;
    int adx = dx < 0 ? -dx : dx, ady = dy < 0 ? -dy : dy;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    int err = (adx > ady ? adx : -ady) / 2, e2;
    for (;;) {
        cv_pixel(cv, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        e2 = err;
        if (e2 > -adx) { err -= ady; x0 += sx; }
        if (e2 <  ady) { err += adx; y0 += sy; }
    }
}

void cv_circle(canvas_t *cv, int cx, int cy, int r, uint16_t color)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        cv_pixel(cv, cx + x, cy + y, color); cv_pixel(cv, cx + y, cy + x, color);
        cv_pixel(cv, cx - y, cy + x, color); cv_pixel(cv, cx - x, cy + y, color);
        cv_pixel(cv, cx - x, cy - y, color); cv_pixel(cv, cx - y, cy - x, color);
        cv_pixel(cv, cx + y, cy - x, color); cv_pixel(cv, cx + x, cy - y, color);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void cv_fill_circle(canvas_t *cv, int cx, int cy, int r, uint16_t color)
{
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r) cv_pixel(cv, cx + dx, cy + dy, color);
}

int cv_char(canvas_t *cv, int x, int y, char c, uint16_t fg, int32_t bg, int scale)
{
    if (scale < 1) scale = 1;
    uint8_t uc = (uint8_t)c;
    if (uc < FONT5X8_FIRST || uc > FONT5X8_LAST) uc = '?';
    const uint8_t *g = &aegis_font5x8[(uc - FONT5X8_FIRST) * FONT5X8_H];
    for (int row = 0; row < FONT5X8_H; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < FONT5X8_W; col++) {
            bool on = bits & (0x80 >> col);
            if (scale == 1) {                       /* the common case */
                if (on)           cv_pixel(cv, x + col, y + row, fg);
                else if (bg >= 0) cv_pixel(cv, x + col, y + row, (uint16_t)bg);
            } else if (on)        cv_fill_rect(cv, x + col * scale, y + row * scale, scale, scale, fg);
            else if (bg >= 0)     cv_fill_rect(cv, x + col * scale, y + row * scale, scale, scale, (uint16_t)bg);
        }
    }
    return x + (FONT5X8_W + 1) * scale;
}

int cv_text(canvas_t *cv, int x, int y, const char *s, uint16_t fg, int32_t bg, int scale)
{
    for (; *s; s++) x = cv_char(cv, x, y, *s, fg, bg, scale);
    return x;
}

int cv_text_width(const char *s, int scale)
{
    if (scale < 1) scale = 1;
    int n = 0; for (; *s; s++) n++;
    return n * (FONT5X8_W + 1) * scale;
}

void cv_text_center(canvas_t *cv, int cx, int y, const char *s, uint16_t fg, int32_t bg, int scale)
{
    cv_text(cv, cx - cv_text_width(s, scale) / 2, y, s, fg, bg, scale);
}

/* ---- the larger credential face ---------------------------------------- */
static int cv_char_big(canvas_t *cv, int x, int y, char c, uint16_t fg, int32_t bg, int scale)
{
    if (scale < 1) scale = 1;
    uint8_t uc = (uint8_t)c;
    if (uc >= 'a' && uc <= 'z') uc = (uint8_t)(uc - 'a' + 'A');   /* face is caps */
    if (uc < FONT7X12_FIRST || uc > FONT7X12_LAST) uc = ' ';
    const uint8_t *g = &aegis_font7x12[(uc - FONT7X12_FIRST) * FONT7X12_H];
    for (int row = 0; row < FONT7X12_H; row++) {
        uint8_t bits = g[row];
        for (int col = 0; col < FONT7X12_W; col++) {
            bool on = bits & (0x80 >> col);
            if (on) cv_fill_rect(cv, x + col * scale, y + row * scale, scale, scale, fg);
            else if (bg >= 0) cv_fill_rect(cv, x + col * scale, y + row * scale, scale, scale, (uint16_t)bg);
        }
    }
    return x + (FONT7X12_W + 1) * scale;
}

int cv_text_big(canvas_t *cv, int x, int y, const char *s, uint16_t fg, int32_t bg, int scale)
{
    for (; s && *s; s++) x = cv_char_big(cv, x, y, *s, fg, bg, scale);
    return x;
}

int cv_text_big_width(const char *s, int scale)
{
    if (scale < 1) scale = 1;
    int n = 0; for (; s && *s; s++) n++;
    return n * (FONT7X12_W + 1) * scale;
}
