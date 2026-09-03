#include "unicode.h"
#include "hid_keys.h"
#include "layout.h"
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

/* macOS types accented characters with OPTION on the ordinary US keyboard.
 *
 * The Option+hex method below needs the "Unicode Hex Input" layout selected in
 * System Settings, and no Mac has that by default. On a stock machine Option+0
 * is "\u00ba" and Option+e is a dead acute, so "Gr\u00fc\u00dfe" came out as
 * accent soup. These sequences are what a Mac user actually presses, so they
 * work on a machine nobody has prepared - which is the point of the device. */
typedef struct { uint32_t cp; char dead; char base; } mac_accent_t;
static const mac_accent_t MAC_ACCENT[] = {
    /* Option+e = acute */
    {0x00E1,'e','a'},{0x00E9,'e','e'},{0x00ED,'e','i'},{0x00F3,'e','o'},{0x00FA,'e','u'},{0x00FD,'e','y'},
    {0x00C1,'e','A'},{0x00C9,'e','E'},{0x00CD,'e','I'},{0x00D3,'e','O'},{0x00DA,'e','U'},{0x00DD,'e','Y'},
    /* Option+u = umlaut */
    {0x00E4,'u','a'},{0x00EB,'u','e'},{0x00EF,'u','i'},{0x00F6,'u','o'},{0x00FC,'u','u'},{0x00FF,'u','y'},
    {0x00C4,'u','A'},{0x00CB,'u','E'},{0x00CF,'u','I'},{0x00D6,'u','O'},{0x00DC,'u','U'},
    /* Option+` = grave */
    {0x00E0,'`','a'},{0x00E8,'`','e'},{0x00EC,'`','i'},{0x00F2,'`','o'},{0x00F9,'`','u'},
    {0x00C0,'`','A'},{0x00C8,'`','E'},{0x00CC,'`','I'},{0x00D2,'`','O'},{0x00D9,'`','U'},
    /* Option+i = circumflex */
    {0x00E2,'i','a'},{0x00EA,'i','e'},{0x00EE,'i','i'},{0x00F4,'i','o'},{0x00FB,'i','u'},
    {0x00C2,'i','A'},{0x00CA,'i','E'},{0x00CE,'i','I'},{0x00D4,'i','O'},{0x00DB,'i','U'},
    /* Option+n = tilde */
    {0x00E3,'n','a'},{0x00F1,'n','n'},{0x00F5,'n','o'},
    {0x00C3,'n','A'},{0x00D1,'n','N'},{0x00D5,'n','O'},
};
/* `key` is the UNSHIFTED character on the physical key, so the shift flag is
 * never ambiguous. */
typedef struct { uint32_t cp; char key; bool shift; } mac_direct_t;
static const mac_direct_t MAC_DIRECT[] = {
    {0x00E7,'c',false},{0x00C7,'c',true},      /* c cedilla   */
    {0x00DF,'s',false},                        /* eszett      */
    {0x00E5,'a',false},{0x00C5,'a',true},      /* a ring      */
    {0x00F8,'o',false},{0x00D8,'o',true},      /* o slash     */
    {0x00E6,'\'',false},{0x00C6,'\'',true},    /* ae ligature */
    {0x0153,'q',false},{0x0152,'q',true},      /* oe ligature */
    {0x00A3,'3',false},{0x00A5,'y',false},     /* pound, yen  */
    {0x20AC,'2',true },{0x00A2,'4',false},     /* euro, cent  */
    {0x00A1,'1',false},{0x00BF,'/',true },     /* inverted ! ?*/
    {0x00AB,'\\',false},{0x00BB,'\\',true},    /* guillemets  */
    {0x2013,'-',false},{0x2014,'-',true },     /* en/em dash  */
    {0x2022,'8',false},{0x00B0,'8',true },     /* bullet, deg */
    {0x2026,';',false},                        /* ellipsis    */
    {0x00A9,'g',false},{0x00AE,'r',false},{0x2122,'2',false},
};

int mac_option_seq(uint32_t cp, ducky_action_t *out, int max)
{
    uint8_t k, m;
    for (size_t i = 0; i < sizeof(MAC_ACCENT) / sizeof(MAC_ACCENT[0]); i++) {
        if (MAC_ACCENT[i].cp != cp) continue;
        if (max < 2) return 0;
        if (!hid_from_ascii_layout(MAC_ACCENT[i].dead, LAYOUT_US, &k, &m)) return 0;
        memset(&out[0], 0, sizeof(out[0]));
        out[0].kind = DUCKY_KEY; out[0].key = k;
        out[0].mods = (uint8_t)(m | HID_MOD_LALT);      /* the dead key */
        if (!hid_from_ascii_layout(MAC_ACCENT[i].base, LAYOUT_US, &k, &m)) return 0;
        memset(&out[1], 0, sizeof(out[1]));
        out[1].kind = DUCKY_KEY; out[1].key = k; out[1].mods = m;   /* then the letter */
        return 2;
    }
    for (size_t i = 0; i < sizeof(MAC_DIRECT) / sizeof(MAC_DIRECT[0]); i++) {
        if (MAC_DIRECT[i].cp != cp) continue;
        if (max < 1) return 0;
        if (!hid_from_ascii_layout(MAC_DIRECT[i].key, LAYOUT_US, &k, &m)) return 0;
        memset(&out[0], 0, sizeof(out[0]));
        out[0].kind = DUCKY_KEY; out[0].key = k;
        out[0].mods = (uint8_t)(m | HID_MOD_LALT |
                                (MAC_DIRECT[i].shift ? HID_MOD_LSHIFT : 0));
        return 1;
    }
    return 0;
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
