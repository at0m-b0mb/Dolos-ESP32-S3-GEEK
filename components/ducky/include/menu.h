/*
 * menu.h - the on-device settings menu, as pure data + logic.
 *
 * The GEEK has no touchscreen: one 1.14" display and one BOOT button. So the
 * whole settings UI is driven by three gestures, and the model below is what
 * they act on:
 *
 *   TAP        move to the next item
 *   HOLD       activate it (toggle a flag, cycle a choice, save, exit)
 *   DOUBLE-TAP open the menu from SAFE, or close it from anywhere
 *
 * Keeping the model here - separate from drawing and from FreeRTOS - means the
 * cycling, labelling and save behaviour are all exercised by host tests.
 */
#ifndef DOLOS_MENU_H
#define DOLOS_MENU_H

#include <stddef.h>
#include "dconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MENU_LAYOUT = 0,   /* target keyboard layout            */
    MENU_OS,           /* target OS for Unicode input       */
    MENU_SPEED,        /* injection speed profile           */
    MENU_DRYRUN,       /* preview without typing            */
    MENU_WIFI,         /* wireless console on/off           */
    MENU_REMOTE_FIRE,  /* allow console-triggered firing    */
    MENU_SAVE,         /* write DOLOS.CFG back to the card  */
    MENU_EXIT,         /* leave the menu                    */
    MENU__COUNT
} menu_item_t;

/* What activating an item asked the caller to do. */
typedef enum { MENU_ACT_NONE = 0, MENU_ACT_SAVE, MENU_ACT_EXIT } menu_action_t;

const char *menu_label(menu_item_t item);

/* Human-readable current value of an item ("US", "ON", "-", ...). */
void menu_value(const dolos_config_t *c, menu_item_t item, char *out, size_t cap);

/* Toggle/cycle the item. Returns what the caller must do (save/exit/nothing). */
menu_action_t menu_activate(dolos_config_t *c, menu_item_t item);

/* True when changing this item only takes effect after a restart, so the UI
 * can say so rather than leaving the operator wondering. */
bool menu_needs_reboot(menu_item_t item);

/* Serialise settings as a DOLOS.CFG the parser round-trips. Returns the length
 * written (excluding the NUL), or 0 if it did not fit. */
size_t config_write_text(const dolos_config_t *c, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* DOLOS_MENU_H */
