/*
 * console_server.h - the secure wireless console (HTTP over the WPA2 SoftAP).
 * Auth: PBKDF2-HMAC-SHA256 creds, cookie sessions, CSRF, lockout, RBAC.
 */
#ifndef DOLOS_CONSOLE_SERVER_H
#define DOLOS_CONSOLE_SERVER_H
#include <stdbool.h>

/* admin_pass may be NULL/empty -> a random one is generated and returned by
 * console_admin_password() so the device can show it on the LCD. */
bool console_server_start(const char *admin_user, const char *admin_pass, bool remote_fire_default);
const char *console_admin_password(void);   /* the effective admin password (for first-run LCD) */
#endif
