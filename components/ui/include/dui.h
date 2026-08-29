/*
 * dui.h - the Dolos on-screen "mission control" for the ESP32-S3-GEEK LCD.
 *
 * Pure C, host-testable. Shows the safety state front and centre so the operator
 * always knows whether the device will type, plus the selected payload, the
 * target keyboard layout, the injection speed, a DRY-RUN badge, and a PIN-entry
 * screen. A persistent "LAB USE ONLY" tag never leaves the screen.
 */
#ifndef DOLOS_DUI_H
#define DOLOS_DUI_H
#include "canvas.h"
#include "menu.h"   /* settings model drawn by the menu screen */

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DUI_SAFE = 0,   /* will not type; hold BOOT to arm            */
    DUI_PINENTRY,   /* dialing the arm PIN                        */
    DUI_ARMED,      /* one more hold fires; tap cancels           */
    DUI_COUNTDOWN,  /* 3-2-1 before it types; tap aborts          */
    DUI_RUNNING,    /* sending the payload; tap aborts            */
    DUI_MENU,       /* settings menu: tap=next, hold=change       */
    DUI_DONE,       /* payload sent                               */
} dui_mode_t;

typedef struct {
    dui_mode_t mode;
    const char *payload_name;
    int  payload_idx;          /* 1-based selection                */
    int  payload_count;
    int  total_lines;
    int  cur_line;
    int  countdown;            /* 3,2,1 for DUI_COUNTDOWN           */
    bool usb_mounted;
    bool dry_run;
    const char *layout;        /* "US","DE",...                    */
    const char *speed;         /* "FAST","BALANCED","RELIABLE"     */
    int  pin_len;              /* expected PIN digits              */
    int  pin_pos;              /* digits committed so far          */
    int  pin_cur;             /* current digit being dialed (1-9)  */
    uint8_t  leds;             /* host LED bitmap: exfil return channel */
    bool     wifi_on;          /* SoftAP + console running               */
    const char *wifi_ssid;     /* AP SSID (shown on SAFE)                */
    const char *wifi_key;      /* AP passphrase to show on SAFE (NULL = hide) */
    const char *admin_user;    /* console username                            */
    const char *admin_pw;      /* console password (NULL to hide)             */
    bool     remote_fire_enabled; /* admin allowed remote fire -> banner  */
    int      menu_sel;         /* highlighted settings row (DUI_MENU)     */
    const dolos_config_t *cfg; /* live settings, shown in the menu        */
    ui_lock_t ui_lock;         /* how much of the UI the button may reach */
    int      lint_problems;    /* >0 = payload has errors, arming blocked */
    int      lint_line;        /* line number of the first problem        */
    const char *lint_msg;      /* short description of it                 */
    uint16_t anim;
} dui_state_t;

void dui_render(canvas_t *cv, const dui_state_t *st);
void dui_render_splash(canvas_t *cv);

#ifdef __cplusplus
}
#endif
#endif /* DOLOS_DUI_H */
