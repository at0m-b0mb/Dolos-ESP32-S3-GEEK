#include "ducky.h"
#include "hid_keys.h"
#include <string.h>
#include <ctype.h>

/* US-layout ASCII -> HID usage, as a direct 128-byte lookup.
 *
 * This replaced a ~40-branch if/switch chain that ran for EVERY character of
 * every STRING. One indexed load is constant time and branch-free, which
 * matters at the 1 ms poll rate the fast speed profile uses: at 8000 chars/s
 * the old chain averaged ~20 comparisons per character.
 *
 * Packing: bit 7 = needs LSHIFT, bits 0..6 = HID usage code (all < 0x80).
 * A zero entry means "not typable on this layout". Generated from - and
 * verified byte-identical to - the original branch logic by the keymap tests.
 */
static const uint8_t US_ASCII[128] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 0x00 . */
    0x00, 0x2B, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 0x08 . */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 0x10 . */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* 0x18 . */
    0x2C, 0x9E, 0xB4, 0xA0, 0xA1, 0xA2, 0xA4, 0x34,  /* 0x20   */
    0xA6, 0xA7, 0xA5, 0xAE, 0x36, 0x2D, 0x37, 0x38,  /* 0x28 ( */
    0x27, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24,  /* 0x30 0 */
    0x25, 0x26, 0xB3, 0x33, 0xB6, 0x2E, 0xB7, 0xB8,  /* 0x38 8 */
    0x9F, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A,  /* 0x40 @ */
    0x8B, 0x8C, 0x8D, 0x8E, 0x8F, 0x90, 0x91, 0x92,  /* 0x48 H */
    0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A,  /* 0x50 P */
    0x9B, 0x9C, 0x9D, 0x2F, 0x31, 0x30, 0xA3, 0xAD,  /* 0x58 X */
    0x35, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A,  /* 0x60 ` */
    0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12,  /* 0x68 h */
    0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1A,  /* 0x70 p */
    0x1B, 0x1C, 0x1D, 0xAF, 0xB1, 0xB0, 0xB5, 0x00,  /* 0x78 x */
};

bool hid_from_ascii(char c, uint8_t *key, uint8_t *mods)
{
    uint8_t idx = (uint8_t)c;
    if (idx & 0x80) return false;              /* non-ASCII: caller uses Unicode */
    uint8_t e = US_ASCII[idx];
    if (!e) return false;
    if (key)  *key  = (uint8_t)(e & 0x7F);
    if (mods) *mods = (e & 0x80) ? HID_MOD_LSHIFT : 0;
    return true;
}

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 0;
    return *a == *b;
}

bool hid_modifier(const char *tok, uint8_t *modbit)
{
    uint8_t m = 0;
    if (ieq(tok, "CTRL") || ieq(tok, "CONTROL"))                 m = HID_MOD_LCTRL;
    else if (ieq(tok, "SHIFT"))                                  m = HID_MOD_LSHIFT;
    else if (ieq(tok, "ALT"))                                    m = HID_MOD_LALT;
    else if (ieq(tok, "GUI") || ieq(tok, "WINDOWS") ||
             ieq(tok, "WIN")  || ieq(tok, "COMMAND") || ieq(tok, "META")) m = HID_MOD_LGUI;
    else return false;
    if (modbit) *modbit = m;
    return true;
}

bool hid_named_key(const char *tok, uint8_t *key)
{
    uint8_t k = 0;
    if      (ieq(tok, "ENTER") || ieq(tok, "RETURN")) k = HID_KEY_ENTER;
    else if (ieq(tok, "ESC") || ieq(tok, "ESCAPE"))   k = HID_KEY_ESC;
    else if (ieq(tok, "TAB"))                         k = HID_KEY_TAB;
    else if (ieq(tok, "SPACE"))                       k = HID_KEY_SPACE;
    else if (ieq(tok, "BACKSPACE") || ieq(tok, "BKSP")) k = HID_KEY_BSPACE;
    else if (ieq(tok, "DELETE") || ieq(tok, "DEL"))   k = HID_KEY_DELETE;
    else if (ieq(tok, "INSERT"))                      k = HID_KEY_INSERT;
    else if (ieq(tok, "HOME"))                        k = HID_KEY_HOME;
    else if (ieq(tok, "END"))                         k = HID_KEY_END;
    else if (ieq(tok, "PAGEUP") || ieq(tok, "PGUP"))  k = HID_KEY_PGUP;
    else if (ieq(tok, "PAGEDOWN") || ieq(tok, "PGDN")) k = HID_KEY_PGDN;
    else if (ieq(tok, "UP") || ieq(tok, "UPARROW"))   k = HID_KEY_UP;
    else if (ieq(tok, "DOWN") || ieq(tok, "DOWNARROW")) k = HID_KEY_DOWN;
    else if (ieq(tok, "LEFT") || ieq(tok, "LEFTARROW")) k = HID_KEY_LEFT;
    else if (ieq(tok, "RIGHT") || ieq(tok, "RIGHTARROW")) k = HID_KEY_RIGHT;
    else if (ieq(tok, "CAPSLOCK") || ieq(tok, "CAPS")) k = HID_KEY_CAPS;
    else if (ieq(tok, "MENU") || ieq(tok, "APP"))     k = HID_KEY_MENU;
    else if (ieq(tok, "PRINTSCREEN") || ieq(tok, "PRTSCR")) k = HID_KEY_PRTSCR;
    else if ((tok[0] == 'F' || tok[0] == 'f') && tok[1]) {
        int n = 0; for (const char *p = tok + 1; *p; p++) {
            if (*p < '0' || *p > '9') { n = 0; break; } n = n * 10 + (*p - '0'); }
        if (n >= 1 && n <= 12) k = (uint8_t)(HID_KEY_F1 + (n - 1));
        else return false;
    }
    else return false;
    if (key) *key = k;
    return true;
}
