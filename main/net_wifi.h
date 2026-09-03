/*
 * net_wifi.h - SoftAP for the Dolos wireless console.
 *
 * Starts a WPA2-protected access point (never open). All console traffic rides
 * inside that WPA2-encrypted link. Off unless enabled in DOLOS.CFG (wifi=ap).
 */
#ifndef DOLOS_NET_WIFI_H
#define DOLOS_NET_WIFI_H
#include <stdbool.h>

/* ssid/pass from config (pass must be >= 8 chars for WPA2). Returns false and
 * starts nothing if the passphrase is too weak - we do not fall back to open. */
bool net_wifi_start_ap(const char *ssid, const char *pass);
const char *net_wifi_ssid(void);   /* actual SSID in use (for the LCD)  */
bool net_wifi_active(void);

/* Take the access point down again. Turning the console off in the settings
 * used to change nothing until the next boot, while the screen claimed it was
 * off - so the setting now actually does what it says. */
void net_wifi_stop_ap(void);

/* ---- upstream ("internet pass-through") ---------------------------------
 *
 * The radio runs AP+STA at once: the console keeps its own access point, and
 * the device ALSO joins a network you nominate, so payload development and
 * anything that needs to reach the internet works without unplugging.
 *
 * Deliberately opt-in and admin-only. Joining a network makes the device
 * reachable from that network and puts its traffic on someone else's wire,
 * which is a decision an operator should take explicitly, not a default.
 *
 * Both radios share one antenna and one channel, so an AP client will see the
 * link stutter while the station is scanning or reconnecting. */
bool net_wifi_sta_start(const char *ssid, const char *pass);
void net_wifi_sta_stop(void);
bool net_wifi_sta_enabled(void);
bool net_wifi_sta_connected(void);
const char *net_wifi_sta_ssid(void);
const char *net_wifi_sta_ip(void);      /* "" until a lease arrives */
#endif
