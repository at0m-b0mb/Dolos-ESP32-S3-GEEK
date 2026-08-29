/*
 * canvas.h - a tiny RGB565 software canvas, pure C and host-testable.
 *
 * The UI layer draws the whole dashboard into an off-screen RGB565 buffer; the
 * device display module then blits that buffer to the ST7789 in one shot. Colors
 * are 16-bit 5-6-5. Every primitive is bounds-checked: a write outside the
 * buffer is dropped AND counted in cv->oob, so a unit test can assert the layout
 * never spills off the 240x135 panel (the same overflow guard Sibyl uses).
 */
#ifndef AEGIS_CANVAS_H
#define AEGIS_CANVAS_H

#include <stdint.h>
#include <stdbool.h>
#include "font5x8.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t *px;     /* caller-owned buffer of w*h pixels */
    int       w, h;
    uint32_t  oob;    /* count of pixel writes rejected as out-of-bounds */
} canvas_t;

/* Pack 8-bit r,g,b into RGB565. */
static inline uint16_t cv_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* A small named palette for the dashboard (blue-team look). */
#define CV_BG      cv_rgb(6, 10, 20)      /* near-black navy            */
#define CV_PANEL   cv_rgb(14, 22, 40)     /* panel fill                 */
#define CV_INK     cv_rgb(210, 224, 245)  /* primary text               */
#define CV_DIM     cv_rgb(96, 116, 150)   /* secondary text / grid      */
#define CV_ACCENT  cv_rgb(64, 156, 255)   /* Aegis blue                 */
#define CV_CLEAR   cv_rgb(64, 200, 140)   /* verdict CLEAR (green)      */
#define CV_ELEV    cv_rgb(240, 190, 70)   /* verdict ELEVATED (amber)   */
#define CV_LIKELY  cv_rgb(240, 80, 80)    /* verdict LIKELY (red)       */

void cv_init(canvas_t *cv, uint16_t *buf, int w, int h);
void cv_clear(canvas_t *cv, uint16_t color);
void cv_pixel(canvas_t *cv, int x, int y, uint16_t color);
void cv_hline(canvas_t *cv, int x, int y, int w, uint16_t color);
void cv_vline(canvas_t *cv, int x, int y, int h, uint16_t color);
void cv_fill_rect(canvas_t *cv, int x, int y, int w, int h, uint16_t color);
void cv_rect(canvas_t *cv, int x, int y, int w, int h, uint16_t color);
void cv_line(canvas_t *cv, int x0, int y0, int x1, int y1, uint16_t color);
void cv_circle(canvas_t *cv, int cx, int cy, int r, uint16_t color);
void cv_fill_circle(canvas_t *cv, int cx, int cy, int r, uint16_t color);

/* Text. bg < 0 means transparent. scale is an integer pixel multiplier (>=1).
 * cv_char advances by (FONT5X8_W+1)*scale. cv_text returns the x after drawing. */
int  cv_char(canvas_t *cv, int x, int y, char c, uint16_t fg, int32_t bg, int scale);
int  cv_text(canvas_t *cv, int x, int y, const char *s, uint16_t fg, int32_t bg, int scale);
int  cv_text_width(const char *s, int scale);
void cv_text_center(canvas_t *cv, int cx, int y, const char *s, uint16_t fg, int32_t bg, int scale);

#ifdef __cplusplus
}
#endif
#endif /* AEGIS_CANVAS_H */
