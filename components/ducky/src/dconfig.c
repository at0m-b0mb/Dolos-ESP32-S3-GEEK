#include "dconfig.h"
#include "unicode.h"   /* os_from_name */
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

void config_defaults(dolos_config_t *c)
{
    /* Zero everything first. The individual assignments below only set the
     * first byte of each string, so without this the tail of every buffer is
     * uninitialised stack - which the console can echo back and the logs can
     * print. Cheap, and it makes the struct deterministic for tests. */
    memset(c, 0, sizeof(*c));
    c->layout = LAYOUT_US;
    c->os = OS_WINDOWS;
    c->speed = SPEED_BALANCED;
    c->dry_run = false;
    c->default_delay_ms = 0;
    c->arm_pin[0] = 0;
    c->usb_vid = 0; c->usb_pid = 0;
    c->usb_mfr[0] = 0; c->usb_product[0] = 0;
    c->wifi_on = true;    /* console on out of the box; credentials shown on the LCD */ c->wifi_ssid[0] = 0; c->wifi_pass[0] = 0;
    c->admin_user[0] = 0; c->admin_pass[0] = 0; c->remote_fire = false;
}

static void cfg_str(char *dst, size_t cap, const char *val)
{ strncpy(dst, val, cap - 1); dst[cap - 1] = 0; }

const char *speed_name(dolos_speed_t s)
{
    switch (s) { case SPEED_FAST: return "FAST"; case SPEED_RELIABLE: return "RELIABLE";
                 default: return "BALANCED"; }
}
uint8_t speed_key_delay_ms(dolos_speed_t s)
{
    /* Per-keystroke half-delay, in milliseconds.
     *
     * These numbers are from hardware, not theory. On a real macOS host, 5 ms
     * ("balanced" as it was) dropped the SAME characters in five consecutive
     * runs - the host coalesces reports that arrive faster than it samples, and
     * no amount of retrying on our side helps, because the report IS accepted;
     * it just never becomes a keystroke. 10 ms typed the identical payload
     * perfectly five times out of five.
     *
     * So the profiles are re-tuned around that measurement, and the default is
     * the one that was observed to work. A tool whose whole job is typing
     * accurately should not ship a default that silently loses characters -
     * a dropped letter in a command is worse than a slower payload. */
    /* Extra settle margin in milliseconds ON TOP of host-clocked pacing.
     * Delivery is confirmed for every report, so even zero is accurate; the
     * slower profiles simply give a host more time to act on a keystroke
     * before the next one arrives. */
    switch (s) {
        case SPEED_FAST:     return 0;    /* host speed, nothing added   */
        case SPEED_RELIABLE: return 8;    /* generous margin             */
        default:             return 2;    /* balanced                    */
    }
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}
static bool truthy(const char *v) { return ieq(v, "on") || ieq(v, "1") || ieq(v, "true") || ieq(v, "yes"); }

ui_lock_t ui_lock_from_name(const char *v)
{
    if (!v) return UI_LOCK_OFF;
    if (ieq(v, "full") || ieq(v, "all")) return UI_LOCK_FULL;
    /* "on" keeps meaning what it meant before the levels existed */
    if (ieq(v, "menu") || truthy(v))     return UI_LOCK_MENU;
    return UI_LOCK_OFF;
}

const char *ui_lock_key(ui_lock_t l)
{
    switch (l) { case UI_LOCK_FULL: return "full"; case UI_LOCK_MENU: return "menu";
                 default: return "off"; }
}

bool config_key_known(const char *key)
{
    static const char *KNOWN[] = {
        "layout", "os", "target_os", "speed", "dryrun", "defaultdelay",
        "armpin", "pin", "ui_lock", "lock_ui", "storage_partition", "msc_partition",
        "usb_vid", "usb_pid", "usb_mfr", "usb_product",
        "wifi", "wifi_ssid", "ssid", "wifi_pass", "wifi_password",
        "admin_user", "admin_pass", "admin_password", "remote_fire",
        NULL
    };
    if (!key) return false;
    for (int i = 0; KNOWN[i]; i++) if (ieq(key, KNOWN[i])) return true;
    return false;
}

void config_parse(const char *text, dolos_config_t *c)
{
    if (!text) return;
    const char *p = text;
    char line[128];
    while (*p) {
        size_t n = 0;
        while (*p && *p != '\n' && n < sizeof(line) - 1) line[n++] = *p++;
        line[n] = 0;
        if (*p == '\n') p++;

        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '#' || *s == ';' || *s == 0) continue;      /* comment / blank */
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = s, *val = eq + 1;
        /* trim key trailing space and value edges */
        char *ke = key + strlen(key); while (ke > key && (ke[-1]==' '||ke[-1]=='\t')) *--ke = 0;
        while (*val == ' ' || *val == '\t') val++;
        char *ve = val + strlen(val); while (ve > val && (ve[-1]==' '||ve[-1]=='\t'||ve[-1]=='\r')) *--ve = 0;

        if      (ieq(key, "layout")) c->layout = layout_from_name(val);
        else if (ieq(key, "os") || ieq(key, "target_os")) c->os = os_from_name(val);
        else if (ieq(key, "speed")) {
            if (ieq(val, "fast")) c->speed = SPEED_FAST;
            else if (ieq(val, "reliable")) c->speed = SPEED_RELIABLE;
            else c->speed = SPEED_BALANCED;
        }
        else if (ieq(key, "dryrun") || ieq(key, "dry_run")) c->dry_run = truthy(val);
        else if (ieq(key, "defaultdelay") || ieq(key, "default_delay")) c->default_delay_ms = (uint32_t)atoi(val);
        else if (ieq(key, "usb_vid") || ieq(key, "vid")) c->usb_vid = (uint16_t)strtol(val, 0, 0);
        else if (ieq(key, "usb_pid") || ieq(key, "pid_id") || ieq(key, "pid")) c->usb_pid = (uint16_t)strtol(val, 0, 0);
        else if (ieq(key, "usb_mfr") || ieq(key, "vendor")) { strncpy(c->usb_mfr, val, sizeof(c->usb_mfr)-1); c->usb_mfr[sizeof(c->usb_mfr)-1]=0; }
        else if (ieq(key, "usb_product") || ieq(key, "product")) { strncpy(c->usb_product, val, sizeof(c->usb_product)-1); c->usb_product[sizeof(c->usb_product)-1]=0; }
        else if (ieq(key, "wifi")) c->wifi_on = ieq(val, "ap") || ieq(val, "on") || truthy(val);
        else if (ieq(key, "wifi_ssid") || ieq(key, "ssid")) cfg_str(c->wifi_ssid, sizeof(c->wifi_ssid), val);
        else if (ieq(key, "wifi_pass") || ieq(key, "wifi_password")) cfg_str(c->wifi_pass, sizeof(c->wifi_pass), val);
        else if (ieq(key, "admin_user")) cfg_str(c->admin_user, sizeof(c->admin_user), val);
        else if (ieq(key, "admin_pass") || ieq(key, "admin_password")) cfg_str(c->admin_pass, sizeof(c->admin_pass), val);
        else if (ieq(key, "remote_fire")) c->remote_fire = truthy(val);
        else if (ieq(key, "storage_partition") || ieq(key, "msc_partition")) {
            int v = atoi(val);
            if (v >= 1 && v <= 4) c->msc_partition = (uint8_t)v;
        }
        else if (ieq(key, "ui_lock") || ieq(key, "lock_ui")) c->ui_lock = ui_lock_from_name(val);
        else if (ieq(key, "armpin") || ieq(key, "pin")) {
            size_t i = 0;
            for (const char *q = val; *q && i < sizeof(c->arm_pin) - 1; q++)
                if (*q >= '1' && *q <= '9') c->arm_pin[i++] = *q;   /* digits 1-9 only */
            c->arm_pin[i] = 0;
        }
    }
}
