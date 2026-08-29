#include "menu.h"
#include "unicode.h"    /* os_name */
#include <stdio.h>
#include <string.h>

const char *menu_label(menu_item_t item)
{
    switch (item) {
        case MENU_LAYOUT:      return "KEYBOARD LAYOUT";
        case MENU_OS:          return "TARGET OS";
        case MENU_SPEED:       return "TYPING SPEED";
        case MENU_DRYRUN:      return "DRY RUN";
        case MENU_WIFI:        return "WIFI CONSOLE";
        case MENU_REMOTE_FIRE: return "REMOTE FIRE";
        case MENU_SAVE:        return "SAVE TO CARD";
        case MENU_EXIT:        return "EXIT";
        default:               return "-";
    }
}

bool menu_needs_reboot(menu_item_t item)
{
    /* The radio is brought up once at boot; everything else applies live. */
    return item == MENU_WIFI;
}

void menu_value(const dolos_config_t *c, menu_item_t item, char *out, size_t cap)
{
    if (!out || cap == 0) return;
    switch (item) {
        case MENU_LAYOUT:      snprintf(out, cap, "%s", layout_name(c->layout)); break;
        case MENU_OS:          snprintf(out, cap, "%s", os_name(c->os));         break;
        case MENU_SPEED:       snprintf(out, cap, "%s", speed_name(c->speed));   break;
        case MENU_DRYRUN:      snprintf(out, cap, "%s", c->dry_run     ? "ON" : "OFF"); break;
        case MENU_WIFI:        snprintf(out, cap, "%s", c->wifi_on     ? "ON" : "OFF"); break;
        case MENU_REMOTE_FIRE: snprintf(out, cap, "%s", c->remote_fire ? "ON" : "OFF"); break;
        default:               out[0] = 0; break;
    }
}

menu_action_t menu_activate(dolos_config_t *c, menu_item_t item)
{
    switch (item) {
        case MENU_LAYOUT:
            c->layout = (kb_layout_t)((c->layout + 1) % LAYOUT__COUNT);
            break;
        case MENU_OS:
            c->os = (target_os_t)((c->os + 1) % 3);
            break;
        case MENU_SPEED:
            c->speed = (dolos_speed_t)((c->speed + 1) % 3);
            break;
        case MENU_DRYRUN:      c->dry_run     = !c->dry_run;     break;
        case MENU_WIFI:        c->wifi_on     = !c->wifi_on;     break;
        case MENU_REMOTE_FIRE: c->remote_fire = !c->remote_fire; break;
        case MENU_SAVE:        return MENU_ACT_SAVE;
        case MENU_EXIT:        return MENU_ACT_EXIT;
        default: break;
    }
    return MENU_ACT_NONE;
}

/* Lower-case spelling of a layout, for the config file the parser reads back. */
static const char *layout_key(kb_layout_t l)
{
    switch (l) {
        case LAYOUT_UK: return "uk"; case LAYOUT_DE: return "de"; case LAYOUT_FR: return "fr";
        case LAYOUT_ES: return "es"; case LAYOUT_IT: return "it"; case LAYOUT_PT: return "pt";
        case LAYOUT_SE: return "se"; case LAYOUT_CH: return "ch"; case LAYOUT_LATAM: return "latam";
        default: return "us";
    }
}
static const char *os_key(target_os_t o)
{
    return o == OS_LINUX ? "linux" : o == OS_MAC ? "mac" : "windows";
}
static const char *speed_key(dolos_speed_t s)
{
    return s == SPEED_FAST ? "fast" : s == SPEED_RELIABLE ? "reliable" : "balanced";
}

size_t config_write_text(const dolos_config_t *c, char *out, size_t cap)
{
    if (!out || cap == 0) return 0;
    /* Written in the same key=value form config_parse() reads, so saving from
     * the menu and reloading at boot round-trip exactly. Secrets are included
     * because this file IS the device's configuration store - it is the SD card
     * that must be protected, not this writer. */
    int n = snprintf(out, cap,
        "# DOLOS.CFG - written by the on-device settings menu\n"
        "layout=%s\n"
        "os=%s\n"
        "speed=%s\n"
        "dryrun=%s\n"
        "defaultdelay=%lu\n"
        "armpin=%s\n"
        "wifi=%s\n"
        "wifi_ssid=%s\n"
        "wifi_pass=%s\n"
        "admin_user=%s\n"
        "admin_pass=%s\n"
        "remote_fire=%s\n"
        "ui_lock=%s\n",
        layout_key(c->layout), os_key(c->os), speed_key(c->speed),
        c->dry_run ? "on" : "off", (unsigned long)c->default_delay_ms,
        c->arm_pin, c->wifi_on ? "ap" : "off",
        c->wifi_ssid, c->wifi_pass, c->admin_user, c->admin_pass,
        c->remote_fire ? "on" : "off", ui_lock_key(c->ui_lock));
    if (n < 0 || (size_t)n >= cap) return 0;      /* truncated: report failure */
    return (size_t)n;
}
