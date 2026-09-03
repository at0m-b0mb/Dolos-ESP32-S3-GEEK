#include "dolos_test.h"
#include "unicode.h"
#include "ducky.h"
#include "hid_keys.h"
#include <string.h>

TEST_MAIN_BEGIN
    ducky_action_t a[32];

    SUITE("unicode: os name parsing");
    {
        CHECK(os_from_name("windows") == OS_WINDOWS, "windows");
        CHECK(os_from_name("linux") == OS_LINUX, "linux");
        CHECK(os_from_name("macos") == OS_MAC, "macos");
        CHECK(os_from_name("osx") == OS_MAC, "osx");
        CHECK(os_from_name(0) == OS_WINDOWS, "null -> windows");
    }

    SUITE("unicode: UTF-8 decode");
    {
        const char *s = "A\xC3\xA9\xE2\x82\xAC";   /* 'A', 'é' U+00E9, '€' U+20AC */
        uint32_t cp; const char *p = s;
        CHECK(utf8_next(&p, &cp) == 1 && cp == 'A', "ascii A");
        CHECK(utf8_next(&p, &cp) == 2 && cp == 0x00E9, "2-byte é");
        CHECK(utf8_next(&p, &cp) == 3 && cp == 0x20AC, "3-byte euro");
        CHECK(utf8_next(&p, &cp) == 0, "end of string");
    }

    SUITE("unicode: Windows Alt+numpad sequence holds Alt across hex");
    {
        int n = unicode_seq(0x00E9, OS_WINDOWS, a, 32);      /* hex 'e9' */
        CHECK(n == 5, "HOLD + KP_PLUS + 2 hex + RELEASE = 5, got %d", n);
        CHECK(a[0].kind == DUCKY_HOLD && a[0].mods == HID_MOD_LALT, "starts by holding Alt");
        CHECK(a[1].kind == DUCKY_KEY && a[1].key == HID_KEY_KP_PLUS, "then numpad +");
        CHECK(a[n-1].kind == DUCKY_RELEASE, "ends by releasing");
    }

    SUITE("unicode: Linux Ctrl+Shift+U sequence");
    {
        int n = unicode_seq(0x20AC, OS_LINUX, a, 32);        /* hex '20ac' */
        CHECK(a[0].kind == DUCKY_HOLD && a[0].mods == (HID_MOD_LCTRL | HID_MOD_LSHIFT), "holds Ctrl+Shift");
        CHECK(a[1].kind == DUCKY_KEY && a[1].key == (HID_KEY_A + 20), "then U");
        CHECK(n == 7, "HOLD + U + 4 hex + RELEASE = 7, got %d", n);
    }

    SUITE("unicode: macOS Option+hex is BMP-only, 4 digits");
    {
        int n = unicode_seq(0x00E9, OS_MAC, a, 32);
        CHECK(n == 6, "HOLD + 4 hex + RELEASE = 6, got %d", n);
        CHECK(a[0].mods == HID_MOD_LALT, "holds Option/Alt");
        CHECK(unicode_seq(0x1F600, OS_MAC, a, 32) == 0, "emoji > U+FFFF unsupported on mac path");
        CHECK(unicode_seq(0x1F600, OS_WINDOWS, a, 32) > 0, "but works on Windows");
    }

    SUITE("ducky: STRING with a non-ASCII char emits a Unicode sequence");
    {
        ducky_state_t st; ducky_state_init(&st); st.target_os = OS_LINUX;
        int n = ducky_parse_line(&st, "STRING a\xC3\xA9", a, 32);   /* 'a' then 'é' */
        CHECK(n >= 6, "one key + a unicode sequence, got %d", n);
        CHECK(a[0].kind == DUCKY_KEY && a[0].key == HID_KEY_A, "first types 'a' directly");
        CHECK(a[1].kind == DUCKY_HOLD, "then the unicode hold begins");
    }

    SUITE("ducky: UNICODE command");
    {
        ducky_state_t st; ducky_state_init(&st); st.target_os = OS_WINDOWS;
        int n = ducky_parse_line(&st, "UNICODE 1F600", a, 32);
        CHECK(n > 0 && a[0].kind == DUCKY_HOLD, "UNICODE emits a held sequence");
        CHECK(ducky_parse_line(&st, "UNICODE U+00E9", a, 32) > 0, "accepts U+ prefix");
    }

    SUITE("macOS: accents use Option dead keys, not the hex layout nobody has");
    {
        ducky_action_t a[8];
        /* u-umlaut = Option+u, then u. Two keystrokes on a stock US Mac. */
        int n = mac_option_seq(0x00FC, a, 8);
        CHECK(n == 2, "u-umlaut is two keystrokes, got %d", n);
        CHECK(a[0].key == (HID_KEY_A + ('u'-'a')) && (a[0].mods & HID_MOD_LALT),
              "first is Option+u (the umlaut dead key)");
        CHECK(a[1].key == (HID_KEY_A + ('u'-'a')) && a[1].mods == 0, "then a plain u");

        n = mac_option_seq(0x00E9, a, 8);          /* e-acute */
        CHECK(n == 2 && a[0].key == (HID_KEY_A + ('e'-'a')) && (a[0].mods & HID_MOD_LALT),
              "e-acute starts with Option+e");
        CHECK(a[1].key == (HID_KEY_A + ('e'-'a')) && a[1].mods == 0, "then a plain e");

        n = mac_option_seq(0x00F1, a, 8);          /* n-tilde */
        CHECK(n == 2 && (a[0].mods & HID_MOD_LALT) && a[0].key == (HID_KEY_A + ('n'-'a')),
              "n-tilde starts with Option+n");

        n = mac_option_seq(0x00DF, a, 8);          /* eszett */
        CHECK(n == 1 && a[0].key == (HID_KEY_A + ('s'-'a')) && (a[0].mods & HID_MOD_LALT),
              "eszett is a single Option+s");

        n = mac_option_seq(0x20AC, a, 8);          /* euro */
        CHECK(n == 1 && (a[0].mods & HID_MOD_LALT) && (a[0].mods & HID_MOD_LSHIFT),
              "euro is Option+Shift+2");

        n = mac_option_seq(0x00A3, a, 8);          /* pound */
        CHECK(n == 1 && (a[0].mods & HID_MOD_LALT) && !(a[0].mods & HID_MOD_LSHIFT),
              "pound is Option+3, with no shift");

        CHECK(mac_option_seq(0x4E2D, a, 8) == 0,
              "a CJK character has no Option sequence and says so");
    }

    SUITE("macOS: a STRING of accented text never falls back to the keypad");
    {
        /* The hex method types on the NUMERIC KEYPAD. Seeing a keypad usage in
         * this output means we are back to the sequence that produced accent
         * soup on a stock Mac. */
        ducky_state_t st; ducky_state_init(&st);
        st.layout = LAYOUT_US; st.target_os = OS_MAC;
        static ducky_action_t a[192];
        int n = ducky_parse_line(&st, "STRING Gr\xc3\xbc" "\xc3\x9f" "e aus M\xc3\xbc" "nchen", a, 192);
        CHECK(n > 0, "the line produced keystrokes");
        /* The Windows hex method always opens with the keypad "+", so that key
         * is the giveaway - the hex digits themselves may be letters. */
        bool keypad = false;
        for (int i = 0; i < n; i++) if (a[i].key == HID_KEY_KP_PLUS) keypad = true;
        CHECK(!keypad, "the keypad hex sequence is not used at all");

        /* and the same text on Windows SHOULD still use the keypad method */
        st.target_os = OS_WINDOWS;
        n = ducky_parse_line(&st, "STRING M\xc3\xbc" "nchen", a, 192);
        keypad = false;
        for (int i = 0; i < n; i++) if (a[i].key == HID_KEY_KP_PLUS) keypad = true;
        CHECK(keypad, "Windows still uses Alt + keypad, which is right for it");
    }
TEST_MAIN_END
