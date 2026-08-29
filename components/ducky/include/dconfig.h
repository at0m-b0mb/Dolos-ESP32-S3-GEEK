/*
 * dconfig.h - Dolos settings, loaded from /sdcard/DOLOS.CFG (key=value lines).
 *
 * Example DOLOS.CFG:
 *     layout=de           # us | uk | de | fr | es
 *     speed=fast          # fast | balanced | reliable
 *     dryrun=off          # on = preview keystrokes, never send them
 *     defaultdelay=40     # ms between commands unless the payload overrides
 *     armpin=231          # optional: tap-count code to arm (digits 1-9), ""=off
 * Pure C, host-testable.
 */
#ifndef DOLOS_DCONFIG_H
#define DOLOS_DCONFIG_H
#include <stdbool.h>
#include <stdint.h>
#include "layout.h"
#include "ducky.h"   /* target_os_t */

typedef enum { SPEED_FAST = 0, SPEED_BALANCED = 1, SPEED_RELIABLE = 2 } dolos_speed_t;

/* How much of the on-device UI the BOOT button may reach.
 *
 * This is a lock against tinkering, not against the operator: arming, firing
 * and the console are unaffected at every level, and the levels are ordered so
 * a comparison reads naturally (`>= UI_LOCK_FULL`). The lock is deliberately
 * NOT one of the menu items - a lock you can switch off from the screen it
 * locks is not a lock. */
typedef enum {
    UI_LOCK_OFF = 0,   /* everything reachable                              */
    UI_LOCK_MENU,      /* no settings menu; payload can still be switched   */
    UI_LOCK_FULL,      /* no menu and no payload switching: one fixed job   */
} ui_lock_t;

/* "off"/"no"/"false"/"0" -> OFF, "full"/"all" -> FULL, anything truthy or
 * "menu" -> MENU. Unknown values are treated as OFF. */
ui_lock_t   ui_lock_from_name(const char *name);
const char *ui_lock_key(ui_lock_t l);      /* "off" | "menu" | "full" */

typedef struct {
    kb_layout_t   layout;
    target_os_t   os;             /* Unicode input-method target OS */
    dolos_speed_t speed;
    bool          dry_run;
    uint32_t      default_delay_ms;
    char          arm_pin[9];     /* digits '1'..'9', empty = no PIN */
    uint16_t      usb_vid;        /* 0 = keep Dolos default          */
    uint16_t      usb_pid;
    char          usb_mfr[24];    /* USB manufacturer string         */
    char          usb_product[32];/* USB product string              */
    /* --- wireless console (v0.3) --- */
    bool          wifi_on;        /* start the SoftAP + console?     */
    char          wifi_ssid[32];  /* AP SSID (default Dolos-XXXX)    */
    char          wifi_pass[64];  /* WPA2 passphrase (>=8, required) */
    char          admin_user[24]; /* console admin username          */
    char          admin_pass[32]; /* console admin password (blank = random on LCD) */
    ui_lock_t     ui_lock;        /* how much of the UI the button may reach   */
    bool          remote_fire;    /* admin default for remote-fire allow (still visible) */
} dolos_config_t;

void config_defaults(dolos_config_t *c);

/* Is this a key the parser understands? The console needs to tell "I do not
 * know that setting" apart from "you set it to the value it already had" -
 * reporting the second as an error made valid changes look broken. */
bool config_key_known(const char *key);
void config_parse(const char *text, dolos_config_t *c);   /* merges over defaults */
const char *speed_name(dolos_speed_t s);
/* per-keystroke half-delay (ms) for each speed profile */
uint8_t speed_key_delay_ms(dolos_speed_t s);
#endif
