/*
 * console_bridge.h - the narrow interface the web console uses to reach the app.
 * Implemented in dolos_main.c (guarded by the app mutex); called by
 * console_server.c. Keeps the HTTP layer and the state machine decoupled.
 */
#ifndef DOLOS_CONSOLE_BRIDGE_H
#define DOLOS_CONSOLE_BRIDGE_H
#include <stdbool.h>
#include <stddef.h>

void bridge_status_json(char *buf, size_t cap);
int  bridge_list_payloads(char *out, size_t cap);            /* JSON array */
int  bridge_read_payload(const char *name, char *buf, size_t cap);
bool bridge_write_payload(const char *name, const char *data, size_t len);
void bridge_get_config_text(char *buf, size_t cap);
bool bridge_set_config(const char *text);
int  bridge_read_audit(char *buf, size_t cap);

bool bridge_remote_select(const char *name);   /* choose active payload   */
bool bridge_remote_fire_enabled(void);
void bridge_set_remote_fire_enabled(bool on);   /* admin toggle (visible)  */
bool bridge_remote_arm(void);                   /* start fire IF enabled   */
void bridge_remote_abort(void);
#endif
