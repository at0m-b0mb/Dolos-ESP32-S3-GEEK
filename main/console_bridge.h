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

/* Structured settings for the console's form controls. */
void bridge_settings_json(char *buf, size_t cap);
/* Apply one setting live. key/value use exactly the DOLOS.CFG vocabulary, so
 * the console and the card cannot drift apart. Returns false if unknown. */
bool bridge_set_setting(const char *key, const char *value);
/* Write the live settings back to the card. */
bool bridge_save_settings(void);
bool bridge_set_config(const char *text);
int  bridge_read_audit(char *buf, size_t cap);

bool bridge_remote_select(const char *name);   /* choose active payload   */
/* Why an arm request was refused, so the console can say something better than
 * "refused". */
typedef enum {
    ARM_OK = 0,
    ARM_ERR_REMOTE_OFF,   /* admin has not enabled remote fire        */
    ARM_ERR_FLASH_MODE,   /* USB-HID never started; cannot type       */
    ARM_ERR_LINT,         /* selected payload has errors              */
    ARM_ERR_BUSY,         /* already armed/firing/running             */
} arm_result_t;

bool bridge_remote_fire_enabled(void);
void bridge_set_remote_fire_enabled(bool on);   /* admin toggle (visible)  */
arm_result_t bridge_remote_arm(void);                   /* start fire IF enabled   */
void bridge_remote_abort(void);
/* Called on the first successful console login. After that the device stops
 * displaying the admin password, because it has served its purpose. */
void bridge_note_console_login(void);

void dolos_factory_reset(void);   /* wipe secrets + config, then restart */
#endif
