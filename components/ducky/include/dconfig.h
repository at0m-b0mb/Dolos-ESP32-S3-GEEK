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

typedef enum { SPEED_FAST = 0, SPEED_BALANCED = 1, SPEED_RELIABLE = 2 } dolos_speed_t;

typedef struct {
    kb_layout_t   layout;
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
    bool          remote_fire;    /* admin default for remote-fire allow (still visible) */
} dolos_config_t;

void config_defaults(dolos_config_t *c);
void config_parse(const char *text, dolos_config_t *c);   /* merges over defaults */
const char *speed_name(dolos_speed_t s);
/* per-keystroke half-delay (ms) for each speed profile */
uint8_t speed_key_delay_ms(dolos_speed_t s);
#endif
