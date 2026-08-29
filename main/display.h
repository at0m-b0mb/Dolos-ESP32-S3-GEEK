/*
 * display.h - ST7789 bring-up for the ESP32-S3-GEEK, exposing a canvas.
 *
 * Owns a 240x135 RGB565 framebuffer (in PSRAM), inits the panel over SPI2 with
 * the GEEK pins/offset from board.h, and blits the framebuffer on flush. The UI
 * layer draws into display_canvas(); the caller calls display_flush() to show it.
 *
 * Display bring-up (landscape orientation, RGB vs BGR, byte order, backlight
 * polarity) is the part most likely to need a one-line tweak on real hardware -
 * see the notes in display.c. Everything else is verified.
 */
#ifndef AEGIS_DISPLAY_H
#define AEGIS_DISPLAY_H
#include <stdbool.h>
#include "canvas.h"

bool      display_init(void);      /* false if panel/PSRAM unavailable (run headless) */
canvas_t *display_canvas(void);    /* the canvas backed by the framebuffer            */
void      display_flush(void);     /* push the framebuffer to the panel               */
#endif
