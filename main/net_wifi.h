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
#endif
