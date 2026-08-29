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
TEST_MAIN_END
