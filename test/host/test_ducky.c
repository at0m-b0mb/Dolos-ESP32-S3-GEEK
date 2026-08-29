#include "dolos_test.h"
#include "ducky.h"
#include "hid_keys.h"
#include <string.h>

TEST_MAIN_BEGIN
    ducky_action_t a[64];
    ducky_state_t st;

    SUITE("ducky: REM and blank lines produce nothing");
    {
        ducky_state_init(&st);
        CHECK(ducky_parse_line(&st, "REM this is a comment", a, 64) == 0, "REM -> 0 actions");
        CHECK(ducky_parse_line(&st, "   ", a, 64) == 0, "blank -> 0 actions");
        CHECK(ducky_parse_line(&st, "# hash comment", a, 64) == 0, "# -> 0 actions");
    }

    SUITE("ducky: STRING emits one key per character, preserving case");
    {
        ducky_state_init(&st);
        int n = ducky_parse_line(&st, "STRING Ab!", a, 64);
        CHECK(n == 3, "3 chars -> 3 actions, got %d", n);
        CHECK(a[0].key == HID_KEY_A && a[0].mods == HID_MOD_LSHIFT, "'A' upper -> shift");
        CHECK(a[1].key == HID_KEY_A + 1 && a[1].mods == 0, "'b' lower -> no shift");
        CHECK(a[2].key == HID_KEY_1 && a[2].mods == HID_MOD_LSHIFT, "'!' -> shift+1");
    }

    SUITE("ducky: STRING keeps internal spaces");
    {
        ducky_state_init(&st);
        int n = ducky_parse_line(&st, "STRING a b", a, 64);
        CHECK(n == 3, "'a',' ','b' -> 3 actions, got %d", n);
        CHECK(a[1].key == HID_KEY_SPACE, "middle char is a real space");
    }

    SUITE("ducky: STRINGLN appends ENTER");
    {
        ducky_state_init(&st);
        int n = ducky_parse_line(&st, "STRINGLN hi", a, 64);
        CHECK(n == 3, "h,i,ENTER -> 3, got %d", n);
        CHECK(a[2].key == HID_KEY_ENTER && a[2].mods == 0, "last action is ENTER");
    }

    SUITE("ducky: DELAY yields a delay action");
    {
        ducky_state_init(&st);
        int n = ducky_parse_line(&st, "DELAY 500", a, 64);
        CHECK(n == 1 && a[0].kind == DUCKY_DELAY && a[0].delay_ms == 500, "DELAY 500");
    }

    SUITE("ducky: modifier combos fold into one chord");
    {
        ducky_state_init(&st);
        int n = ducky_parse_line(&st, "GUI r", a, 64);
        CHECK(n == 1 && a[0].mods == HID_MOD_LGUI && a[0].key == HID_KEY_A + 17, "GUI r = win+R");

        n = ducky_parse_line(&st, "CTRL ALT DELETE", a, 64);
        CHECK(n == 1 && a[0].key == HID_KEY_DELETE &&
              a[0].mods == (HID_MOD_LCTRL | HID_MOD_LALT), "CTRL ALT DELETE folds mods");

        n = ducky_parse_line(&st, "ENTER", a, 64);
        CHECK(n == 1 && a[0].key == HID_KEY_ENTER && a[0].mods == 0, "bare ENTER");

        n = ducky_parse_line(&st, "GUI", a, 64);
        CHECK(n == 1 && a[0].mods == HID_MOD_LGUI && a[0].key == 0, "lone GUI = modifier-only chord");
    }

    SUITE("ducky: DEFAULTDELAY sets state, not an action; REPEAT sets repeat count");
    {
        ducky_state_init(&st);
        CHECK(ducky_parse_line(&st, "DEFAULTDELAY 200", a, 64) == 0, "DEFAULTDELAY -> 0 actions");
        CHECK(st.default_delay_ms == 200, "default delay stored");
        (void)ducky_parse_line(&st, "STRING hello", a, 64);
        CHECK(ducky_parse_line(&st, "REPEAT 3", a, 64) == 0, "REPEAT -> 0 actions");
        CHECK(st.repeat == 3, "repeat count captured");
        CHECK(strcmp(st.last_cmd, "STRING hello") == 0, "last_cmd is the previous real command");
    }

    SUITE("ducky: garbage tokens are rejected, not typed blindly");
    {
        ducky_state_init(&st);
        CHECK(ducky_parse_line(&st, "FLARP zonk", a, 64) == 0, "unknown combo -> 0 actions");
    }
TEST_MAIN_END
