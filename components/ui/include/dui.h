/*
 * dui.h - the Dolos on-screen "mission control" for the ESP32-S3-GEEK LCD.
 *
 * Pure C, host-testable, draws into a canvas. It shows the safety state front
 * and centre so the operator always knows whether the device will type: SAFE is
 * calm green, ARMED amber, the COUNTDOWN a big red number, RUNNING shows live
 * progress, DONE confirms. A persistent "LAB USE ONLY" tag never leaves screen.
 */
#ifndef DOLOS_DUI_H
#define DOLOS_DUI_H
#include "canvas.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DUI_SAFE = 0,   /* will not type; hold BOOT to arm            */
    DUI_ARMED,      /* one more hold fires; tap cancels           */
    DUI_COUNTDOWN,  /* 3-2-1 before it types; tap aborts          */
    DUI_RUNNING,    /* sending the payload; tap aborts            */
    DUI_DONE,       /* payload sent                               */
} dui_mode_t;

typedef struct {
    dui_mode_t mode;
    const char *payload_name;  /* e.g. "PAYLOAD.TXT" or "demo"    */
    int  total_lines;
    int  cur_line;
    int  countdown;            /* 3,2,1 for DUI_COUNTDOWN         */
    bool usb_mounted;          /* host has enumerated us          */
    uint16_t anim;            /* free-running animation phase    */
} dui_state_t;

void dui_render(canvas_t *cv, const dui_state_t *st);
void dui_render_splash(canvas_t *cv);

#ifdef __cplusplus
}
#endif
#endif /* DOLOS_DUI_H */
