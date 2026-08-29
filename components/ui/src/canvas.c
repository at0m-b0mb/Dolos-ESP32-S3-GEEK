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
    int n = cv->w * cv->h;
    for (int i = 0; i < n; i++) cv->px[i] = color;
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
    for (int j = 0; j < h; j++)
        for (int i = 0; i < w; i++) cv_pixel(cv, x + i, y + j, color);
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
            if (on) cv_fill_rect(cv, x + col * scale, y + row * scale, scale, scale, fg);
            else if (bg >= 0) cv_fill_rect(cv, x + col * scale, y + row * scale, scale, scale, (uint16_t)bg);
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
