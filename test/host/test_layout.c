#include "dolos_test.h"
#include "layout.h"
#include "hid_keys.h"

TEST_MAIN_BEGIN
    uint8_t k, m;

    SUITE("layout: names round-trip");
    {
        CHECK(layout_from_name("de") == LAYOUT_DE, "de");
        CHECK(layout_from_name("FR") == LAYOUT_FR, "case-insensitive");
        CHECK(layout_from_name("zz") == LAYOUT_US, "unknown -> US");
        CHECK(layout_from_name(0) == LAYOUT_US, "null -> US");
    }

    SUITE("layout: US is the plain keymap");
    {
        CHECK(hid_from_ascii_layout('a', LAYOUT_US, &k, &m) && k == HID_KEY_A && m == 0, "US a");
        CHECK(hid_from_ascii_layout('!', LAYOUT_US, &k, &m) && k == HID_KEY_1 && m == HID_MOD_LSHIFT, "US !");
    }

    SUITE("layout: DE swaps Y and Z");
    {
        hid_from_ascii_layout('z', LAYOUT_DE, &k, &m);
        CHECK(k == 0x1C, "DE 'z' uses the US-Y scan code (0x1C), got 0x%02X", k);
        hid_from_ascii_layout('y', LAYOUT_DE, &k, &m);
        CHECK(k == 0x1D, "DE 'y' uses the US-Z scan code (0x1D), got 0x%02X", k);
        CHECK(hid_from_ascii_layout('a', LAYOUT_DE, &k, &m) && k == HID_KEY_A, "DE 'a' unchanged");
    }

    SUITE("layout: FR AZERTY swaps A/Q and shifts the digit row");
    {
        hid_from_ascii_layout('a', LAYOUT_FR, &k, &m);
        CHECK(k == (HID_KEY_A + 16), "FR 'a' uses the US-Q scan code, got 0x%02X", k);
        hid_from_ascii_layout('1', LAYOUT_FR, &k, &m);
        CHECK(k == HID_KEY_1 && m == HID_MOD_LSHIFT, "FR '1' needs shift");
    }

    SUITE("layout: UK moves the double-quote");
    {
        hid_from_ascii_layout('"', LAYOUT_UK, &k, &m);
        CHECK(k == (HID_KEY_1 + 1) && m == HID_MOD_LSHIFT, "UK '\"' = shift+2, got 0x%02X m%u", k, m);
    }
TEST_MAIN_END
