#include "dolos_test.h"
#include "ducky.h"
#include "hid_keys.h"

TEST_MAIN_BEGIN
    uint8_t k, m;

    SUITE("keymap: letters, shift, digits");
    {
        CHECK(hid_from_ascii('a', &k, &m) && k == HID_KEY_A && m == 0, "a -> A, no shift");
        CHECK(hid_from_ascii('A', &k, &m) && k == HID_KEY_A && m == HID_MOD_LSHIFT, "A -> A+shift");
        CHECK(hid_from_ascii('z', &k, &m) && k == HID_KEY_A + 25, "z is A+25");
        CHECK(hid_from_ascii('1', &k, &m) && k == HID_KEY_1 && m == 0, "1 no shift");
        CHECK(hid_from_ascii('0', &k, &m) && k == HID_KEY_0, "0 key");
    }

    SUITE("keymap: shifted punctuation lands on the right base key");
    {
        CHECK(hid_from_ascii('!', &k, &m) && k == HID_KEY_1 && m == HID_MOD_LSHIFT, "! = shift+1");
        CHECK(hid_from_ascii(')', &k, &m) && k == HID_KEY_0 && m == HID_MOD_LSHIFT, ") = shift+0");
        CHECK(hid_from_ascii('/', &k, &m) && k == HID_KEY_SLASH && m == 0, "/ no shift");
        CHECK(hid_from_ascii('?', &k, &m) && k == HID_KEY_SLASH && m == HID_MOD_LSHIFT, "? = shift+/");
        CHECK(hid_from_ascii(' ', &k, &m) && k == HID_KEY_SPACE, "space maps");
        CHECK(!hid_from_ascii('\x01', &k, &m), "control byte is unmapped");
    }

    SUITE("keymap: modifiers + named keys");
    {
        CHECK(hid_modifier("CTRL", &m) && m == HID_MOD_LCTRL, "CTRL");
        CHECK(hid_modifier("control", &m) && m == HID_MOD_LCTRL, "control (case-insensitive alias)");
        CHECK(hid_modifier("GUI", &m) && m == HID_MOD_LGUI, "GUI");
        CHECK(hid_modifier("WINDOWS", &m) && m == HID_MOD_LGUI, "WINDOWS = GUI");
        CHECK(!hid_modifier("BANANA", &m), "unknown is not a modifier");
        CHECK(hid_named_key("ENTER", &k) && k == HID_KEY_ENTER, "ENTER");
        CHECK(hid_named_key("F5", &k) && k == HID_KEY_F1 + 4, "F5");
        CHECK(hid_named_key("DEL", &k) && k == HID_KEY_DELETE, "DEL alias");
        /* F13-F24 are real HID usages (0x68-0x73); F25 is where the row ends. */
        CHECK(hid_named_key("F13", &k) && k == 0x68, "F13 = 0x68");
        CHECK(hid_named_key("F24", &k) && k == 0x73, "F24 = 0x73");
        CHECK(!hid_named_key("F25", &k), "F25 is past the end of the row");
        CHECK(!hid_named_key("F0", &k), "there is no F0");
    }
TEST_MAIN_END
