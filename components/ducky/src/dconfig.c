#include "dconfig.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

void config_defaults(dolos_config_t *c)
{
    c->layout = LAYOUT_US;
    c->speed = SPEED_BALANCED;
    c->dry_run = false;
    c->default_delay_ms = 0;
    c->arm_pin[0] = 0;
    c->usb_vid = 0; c->usb_pid = 0;
    c->usb_mfr[0] = 0; c->usb_product[0] = 0;
}

const char *speed_name(dolos_speed_t s)
{
    switch (s) { case SPEED_FAST: return "FAST"; case SPEED_RELIABLE: return "RELIABLE";
                 default: return "BALANCED"; }
}
uint8_t speed_key_delay_ms(dolos_speed_t s)
{
    switch (s) { case SPEED_FAST: return 1; case SPEED_RELIABLE: return 8; default: return 3; }
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}
static bool truthy(const char *v) { return ieq(v, "on") || ieq(v, "1") || ieq(v, "true") || ieq(v, "yes"); }

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
        else if (ieq(key, "armpin") || ieq(key, "pin")) {
            size_t i = 0;
            for (const char *q = val; *q && i < sizeof(c->arm_pin) - 1; q++)
                if (*q >= '1' && *q <= '9') c->arm_pin[i++] = *q;   /* digits 1-9 only */
            c->arm_pin[i] = 0;
        }
    }
}
