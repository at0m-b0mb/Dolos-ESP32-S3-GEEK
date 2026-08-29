#include "unicode.h"
#include "hid_keys.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

target_os_t os_from_name(const char *n)
{
    if (!n) return OS_WINDOWS;
    char b[8] = {0};
    for (int i = 0; i < 7 && n[i]; i++) b[i] = (char)tolower((unsigned char)n[i]);
    if (!strncmp(b, "lin", 3)) return OS_LINUX;
    if (!strncmp(b, "mac", 3) || !strncmp(b, "osx", 3)) return OS_MAC;
    return OS_WINDOWS;
}
const char *os_name(target_os_t os)
{
    return os == OS_LINUX ? "LINUX" : os == OS_MAC ? "MAC" : "WINDOWS";
}

int utf8_next(const char **p, uint32_t *cp)
{
    const unsigned char *s = (const unsigned char *)*p;
    if (!*s) { *cp = 0; return 0; }
    uint32_t c; int len;
    if (s[0] < 0x80)              { c = s[0];        len = 1; }
    else if ((s[0] & 0xE0) == 0xC0) { c = s[0] & 0x1F; len = 2; }
    else if ((s[0] & 0xF0) == 0xE0) { c = s[0] & 0x0F; len = 3; }
    else if ((s[0] & 0xF8) == 0xF0) { c = s[0] & 0x07; len = 4; }
    else { *cp = 0xFFFD; *p = (const char *)(s + 1); return 1; }
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) { *cp = 0xFFFD; *p = (const char *)(s + 1); return 1; }
        c = (c << 6) | (s[i] & 0x3F);
    }
    *cp = c; *p = (const char *)(s + len); return len;
}

static uint8_t hex_regular(char h) { uint8_t k, m; hid_from_ascii(h, &k, &m); return k; }
static uint8_t hex_numpad(char h)
{
    if (h >= '0' && h <= '9') return h == '0' ? HID_KEY_KP0 : (uint8_t)(HID_KEY_KP1 + (h - '1'));
    return (uint8_t)(HID_KEY_A + (h - 'a'));   /* a-f typed as letter keys while Alt held */
}

int unicode_seq(uint32_t cp, target_os_t os, ducky_action_t *out, int max)
{
    char hex[9];
    int n = 0;
#define PUSH(K, M, KEY) do { \
        if (n >= max) return 0; \
        memset(&out[n], 0, sizeof(out[n])); \
        out[n].kind = (K); out[n].mods = (M); out[n].key = (KEY); n++; \
    } while (0)

    if (os == OS_WINDOWS) {
        snprintf(hex, sizeof(hex), "%x", (unsigned)cp);
        PUSH(DUCKY_HOLD, HID_MOD_LALT, 0);
        PUSH(DUCKY_KEY, 0, HID_KEY_KP_PLUS);
        for (char *h = hex; *h; h++) PUSH(DUCKY_KEY, 0, hex_numpad(*h));
        PUSH(DUCKY_RELEASE, 0, 0);
    } else if (os == OS_LINUX) {
        snprintf(hex, sizeof(hex), "%x", (unsigned)cp);
        PUSH(DUCKY_HOLD, HID_MOD_LCTRL | HID_MOD_LSHIFT, 0);
        PUSH(DUCKY_KEY, 0, (uint8_t)(HID_KEY_A + 20));   /* 'u' */
        for (char *h = hex; *h; h++) PUSH(DUCKY_KEY, 0, hex_regular(*h));
        PUSH(DUCKY_RELEASE, 0, 0);
    } else { /* OS_MAC: Option + 4 hex; BMP only */
        if (cp > 0xFFFF) return 0;
        snprintf(hex, sizeof(hex), "%04x", (unsigned)cp);
        PUSH(DUCKY_HOLD, HID_MOD_LALT, 0);
        for (char *h = hex; *h; h++) PUSH(DUCKY_KEY, 0, hex_regular(*h));
        PUSH(DUCKY_RELEASE, 0, 0);
    }
    return n;
#undef PUSH
}
