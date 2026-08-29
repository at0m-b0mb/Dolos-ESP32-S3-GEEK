#include "dolos_test.h"
#include "layout.h"
#include "ducky.h"
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

    SUITE("layout: accented characters are KEYS, not Unicode escapes");
    {
        uint8_t k, m;
        /* On a German keyboard a-umlaut is one keystroke. Typing it through the
         * OS Unicode method instead costs seven reports and needs Num Lock and
         * a registry setting - and fails outright on a login screen. */
        CHECK(layout_utf8_key(LAYOUT_DE, 0x00E4, &k, &m) && k == HID_KEY_QUOTE && m == 0,
              "DE a-umlaut is the apostrophe key, unshifted");
        CHECK(layout_utf8_key(LAYOUT_DE, 0x00C4, &k, &m) && m == HID_MOD_LSHIFT,
              "DE capital A-umlaut is the same key shifted");
        CHECK(layout_utf8_key(LAYOUT_DE, 0x00DF, &k, &m) && k == HID_KEY_MINUS,
              "DE sharp-s is the minus key");
        CHECK(layout_utf8_key(LAYOUT_FR, 0x00E9, &k, &m) && k == HID_KEY_1 + 1 && m == 0,
              "FR e-acute is the 2 key, unshifted");
        CHECK(layout_utf8_key(LAYOUT_ES, 0x00F1, &k, &m) && k == HID_KEY_SEMI,
              "ES enye is the semicolon key");
        CHECK(layout_utf8_key(LAYOUT_IT, 0x00E8, &k, &m) && k == HID_KEY_LBRACK,
              "IT e-grave is the bracket key");

        /* AltGr characters carry the right modifier */
        CHECK(layout_utf8_key(LAYOUT_DE, 0x20AC, &k, &m) && (m & HID_MOD_RALT),
              "DE euro needs AltGr");

        /* US has no such keys, so those characters must fall back to the OS */
        CHECK(!layout_utf8_key(LAYOUT_US, 0x00E4, &k, &m), "US has no a-umlaut key");
        CHECK(!layout_utf8_key(LAYOUT_DE, 0x65E5, &k, &m), "no CJK on a German keyboard");
    }

    SUITE("layout: STRING prefers the layout key over the OS Unicode method");
    {
        ducky_action_t a[64];
        ducky_state_t st; ducky_state_init(&st);

        /* German target: "Grusse" with an umlaut is 6 keystrokes, not 5 + a
         * seven-report Alt sequence. */
        st.layout = LAYOUT_DE; st.target_os = OS_WINDOWS;
        int n = ducky_parse_line(&st, "STRING \xc3\xa4", a, 64);
        CHECK(n == 1, "one action on a layout that has the key, got %d", n);
        CHECK(a[0].kind == DUCKY_KEY && a[0].key == HID_KEY_QUOTE, "and it is a plain keypress");

        /* US target: the same character must still type, via the OS method */
        st.layout = LAYOUT_US;
        n = ducky_parse_line(&st, "STRING \xc3\xa4", a, 64);
        CHECK(n > 1, "falls back to the OS sequence on US, got %d actions", n);
        CHECK(a[0].kind == DUCKY_HOLD, "which starts by holding Alt");
    }

    SUITE("layout: dead keys - accent, then the letter");
    {
        uint8_t dk, dm, bk, bm;
        /* Spanish writes accented vowels constantly, and none of them are keys:
         * "a-acute" is the apostrophe key followed by A. */
        CHECK(layout_utf8_combo(LAYOUT_ES, 0x00E1, &dk, &dm, &bk, &bm),
              "ES a-acute is a dead-key sequence");
        CHECK(dk == HID_KEY_QUOTE && dm == 0, "the accent is the apostrophe key");
        CHECK(bk == HID_KEY_A && bm == 0, "then a plain 'a'");
        CHECK(layout_utf8_combo(LAYOUT_ES, 0x00C1, &dk, &dm, &bk, &bm) && bm == HID_MOD_LSHIFT,
              "the capital shifts only the LETTER, not the accent");
        CHECK(layout_utf8_combo(LAYOUT_DE, 0x00E9, &dk, &dm, &bk, &bm) && dk == HID_KEY_EQUAL,
              "DE puts the acute accent right of zero");
        CHECK(layout_utf8_combo(LAYOUT_PT, 0x00E3, &dk, &dm, &bk, &bm) && dk == HID_KEY_BSLASH,
              "PT a-tilde uses the tilde dead key");
        CHECK(!layout_utf8_combo(LAYOUT_US, 0x00E1, &dk, &dm, &bk, &bm),
              "US has no dead keys - it falls back to the OS method");

        /* and the emitter uses it: two keystrokes, not a seven-report escape */
        ducky_action_t a[64];
        ducky_state_t st; ducky_state_init(&st);
        st.layout = LAYOUT_ES; st.target_os = OS_WINDOWS;
        int n = ducky_parse_line(&st, "STRING \xc3\xa1", a, 64);
        CHECK(n == 2, "a-acute is two keystrokes on ES, got %d", n);
        CHECK(a[0].key == HID_KEY_QUOTE && a[1].key == HID_KEY_A, "accent then letter");
    }

    SUITE("layout: Nordic and UK now have their own keys");
    {
        uint8_t k, m;
        CHECK(layout_utf8_key(LAYOUT_SE, 0x00E5, &k, &m) && k == HID_KEY_LBRACK,
              "SE a-ring is a key");
        CHECK(layout_utf8_key(LAYOUT_SE, 0x00F6, &k, &m) && k == HID_KEY_SEMI, "SE o-umlaut");
        CHECK(layout_utf8_key(LAYOUT_UK, 0x00A3, &k, &m) && m == HID_MOD_LSHIFT,
              "UK pound is shift+3");
    }
TEST_MAIN_END
