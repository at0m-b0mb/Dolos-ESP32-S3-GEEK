#include "ducky.h"
#include "hid_keys.h"
#include <string.h>
#include <ctype.h>

/* US-layout ASCII -> (key, needs-shift). Returns false for unmapped bytes. */
bool hid_from_ascii(char c, uint8_t *key, uint8_t *mods)
{
    uint8_t k = 0, sh = 0;
    if (c >= 'a' && c <= 'z')      { k = HID_KEY_A + (uint8_t)(c - 'a'); }
    else if (c >= 'A' && c <= 'Z') { k = HID_KEY_A + (uint8_t)(c - 'A'); sh = 1; }
    else if (c >= '1' && c <= '9') { k = HID_KEY_1 + (uint8_t)(c - '1'); }
    else switch (c) {
        case '0': k = HID_KEY_0; break;
        case ' ': k = HID_KEY_SPACE; break;
        case '\n': k = HID_KEY_ENTER; break;
        case '\t': k = HID_KEY_TAB; break;
        case '-': k = HID_KEY_MINUS; break;   case '_': k = HID_KEY_MINUS; sh = 1; break;
        case '=': k = HID_KEY_EQUAL; break;   case '+': k = HID_KEY_EQUAL; sh = 1; break;
        case '[': k = HID_KEY_LBRACK; break;  case '{': k = HID_KEY_LBRACK; sh = 1; break;
        case ']': k = HID_KEY_RBRACK; break;  case '}': k = HID_KEY_RBRACK; sh = 1; break;
        case '\\': k = HID_KEY_BSLASH; break; case '|': k = HID_KEY_BSLASH; sh = 1; break;
        case ';': k = HID_KEY_SEMI; break;    case ':': k = HID_KEY_SEMI; sh = 1; break;
        case '\'': k = HID_KEY_QUOTE; break;  case '"': k = HID_KEY_QUOTE; sh = 1; break;
        case '`': k = HID_KEY_GRAVE; break;   case '~': k = HID_KEY_GRAVE; sh = 1; break;
        case ',': k = HID_KEY_COMMA; break;   case '<': k = HID_KEY_COMMA; sh = 1; break;
        case '.': k = HID_KEY_DOT; break;     case '>': k = HID_KEY_DOT; sh = 1; break;
        case '/': k = HID_KEY_SLASH; break;   case '?': k = HID_KEY_SLASH; sh = 1; break;
        case '!': k = HID_KEY_1; sh = 1; break;
        case '@': k = HID_KEY_1 + 1; sh = 1; break;   /* 2 */
        case '#': k = HID_KEY_1 + 2; sh = 1; break;   /* 3 */
        case '$': k = HID_KEY_1 + 3; sh = 1; break;   /* 4 */
        case '%': k = HID_KEY_1 + 4; sh = 1; break;   /* 5 */
        case '^': k = HID_KEY_1 + 5; sh = 1; break;   /* 6 */
        case '&': k = HID_KEY_1 + 6; sh = 1; break;   /* 7 */
        case '*': k = HID_KEY_1 + 7; sh = 1; break;   /* 8 */
        case '(': k = HID_KEY_1 + 8; sh = 1; break;   /* 9 */
        case ')': k = HID_KEY_0; sh = 1; break;       /* 0 */
        default: return false;
    }
    if (key)  *key  = k;
    if (mods) *mods = sh ? HID_MOD_LSHIFT : 0;
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
