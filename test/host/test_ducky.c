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

    SUITE("ducky: mouse + media commands");
    {
        ducky_state_init(&st);
        int n = ducky_parse_line(&st, "MOUSEMOVE 20 -5", a, 64);
        CHECK(n == 1 && a[0].kind == DUCKY_MOUSE && a[0].mx == 20 && a[0].my == -5, "MOUSEMOVE 20 -5");
        n = ducky_parse_line(&st, "MOUSECLICK RIGHT", a, 64);
        CHECK(n == 1 && a[0].kind == DUCKY_MOUSE && a[0].buttons == 2, "MOUSECLICK RIGHT");
        n = ducky_parse_line(&st, "MOUSEWHEEL -3", a, 64);
        CHECK(n == 1 && a[0].kind == DUCKY_MOUSE && a[0].wheel == -3, "MOUSEWHEEL -3");
        n = ducky_parse_line(&st, "MEDIA VOLUP", a, 64);
        CHECK(n == 1 && a[0].kind == DUCKY_CONSUMER && a[0].consumer == 0xE9, "MEDIA VOLUP");
        CHECK(ducky_parse_line(&st, "MEDIA NONSENSE", a, 64) == 0, "unknown media -> nothing");
    }

    SUITE("lock keys: Caps Lock inverts letters, and only letters");
    {
        /* Caps Lock is the operating system's state, not the keyboard's, so a
         * payload typing "Hello" into a host with it on produces "hELLO"
         * unless the shift bit is flipped for letters. */
        const uint8_t CAPS = HID_LED_CAPSLOCK;
        CHECK(ducky_apply_caps(HID_KEY_A, 0, 0) == 0,
              "caps off: a lowercase letter is unchanged");
        CHECK(ducky_apply_caps(HID_KEY_A, 0, CAPS) == HID_MOD_LSHIFT,
              "caps on: an unshifted letter gains shift, so it still types lowercase");
        CHECK(ducky_apply_caps(HID_KEY_A, HID_MOD_LSHIFT, CAPS) == 0,
              "caps on: a shifted letter loses shift, so it still types uppercase");

        /* digits and punctuation must never be touched: Caps Lock does not
         * affect them, and flipping shift would turn 1 into ! */
        CHECK(ducky_apply_caps(HID_KEY_1, 0, CAPS) == 0, "digits are unaffected");
        CHECK(ducky_apply_caps(HID_KEY_1, HID_MOD_LSHIFT, CAPS) == HID_MOD_LSHIFT,
              "shifted digits are unaffected");
        CHECK(ducky_apply_caps(HID_KEY_MINUS, 0, CAPS) == 0, "punctuation is unaffected");
        CHECK(ducky_apply_caps(HID_KEY_ENTER, 0, CAPS) == 0, "named keys are unaffected");

        /* other modifiers survive the flip */
        CHECK(ducky_apply_caps(HID_KEY_A, HID_MOD_LCTRL, CAPS) == (HID_MOD_LCTRL|HID_MOD_LSHIFT),
              "ctrl is preserved while shift flips");
        /* the last letter of the alphabet is still a letter */
        CHECK(ducky_apply_caps(HID_KEY_A + 25, 0, CAPS) == HID_MOD_LSHIFT, "Z is covered");
        CHECK(ducky_apply_caps(HID_KEY_A + 26, 0, CAPS) == 0, "the key after Z is not");
    }

    SUITE("STRINGDELAY paces the characters of a line, not the whole payload");
    {
        ducky_state_t st; ducky_state_init(&st);
        CHECK(st.string_delay_ms == 0, "off by default");

        /* without it, five characters are five key actions */
        int n = ducky_parse_line(&st, "STRING abcde", a, 64);
        CHECK(n == 5, "5 chars -> 5 actions, got %d", n);

        /* with it, a delay is interleaved BETWEEN characters: 5 keys + 4 gaps */
        ducky_parse_line(&st, "STRINGDELAY 25", a, 64);
        CHECK(st.string_delay_ms == 25, "STRINGDELAY parsed, got %lu",
              (unsigned long)st.string_delay_ms);
        n = ducky_parse_line(&st, "STRING abcde", a, 64);
        CHECK(n == 9, "5 keys + 4 delays = 9 actions, got %d", n);
        CHECK(a[0].kind == DUCKY_KEY,   "starts with a key, not a delay");
        CHECK(a[1].kind == DUCKY_DELAY && a[1].delay_ms == 25, "delay between characters");
        CHECK(a[8].kind == DUCKY_KEY,   "ends with a key, not a trailing delay");

        /* a single character gets no delay at all */
        n = ducky_parse_line(&st, "STRING z", a, 64);
        CHECK(n == 1, "one character needs no pacing, got %d", n);

        /* and it can be turned back off */
        ducky_parse_line(&st, "STRINGDELAY 0", a, 64);
        n = ducky_parse_line(&st, "STRING abcde", a, 64);
        CHECK(n == 5, "STRINGDELAY 0 restores plain typing, got %d", n);
    }

    SUITE("DuckyScript: hyphenated chords, the classic 1.0 form");
    {
        ducky_state_t st; ducky_state_init(&st);
        /* "CTRL-ALT-DELETE" must mean the same as "CTRL ALT DELETE" */
        int n = ducky_parse_line(&st, "CTRL-ALT-DELETE", a, 8);
        CHECK(n == 1, "one chord action, got %d", n);
        CHECK(a[0].mods == (HID_MOD_LCTRL | HID_MOD_LALT), "both modifiers set");
        CHECK(a[0].key == HID_KEY_DELETE, "and the DELETE key");

        ducky_parse_line(&st, "CTRL ALT DELETE", a, 8);
        uint8_t spaced_mods = a[0].mods, spaced_key = a[0].key;
        ducky_parse_line(&st, "CTRL-ALT-DELETE", a, 8);
        CHECK(a[0].mods == spaced_mods && a[0].key == spaced_key,
              "hyphenated and spaced forms are identical");

        ducky_parse_line(&st, "GUI-r", a, 8);
        CHECK(a[0].mods == HID_MOD_LGUI && a[0].key == (HID_KEY_A + 17), "GUI-r");
        ducky_parse_line(&st, "ALT-F4", a, 8);
        CHECK(a[0].mods == HID_MOD_LALT && a[0].key == HID_KEY_F1 + 3, "ALT-F4");

        /* a lone minus is still the minus KEY, not a separator */
        n = ducky_parse_line(&st, "STRING -", a, 8);
        CHECK(n == 1 && a[0].key == HID_KEY_MINUS, "a bare '-' still types");
    }

    SUITE("DuckyScript: REM_BLOCK swallows everything until END_REM");
    {
        ducky_state_t st; ducky_state_init(&st);
        CHECK(ducky_parse_line(&st, "REM_BLOCK", a, 8) == 0, "block opens silently");
        CHECK(ducky_parse_line(&st, "STRING this must not type", a, 8) == 0,
              "commands inside a block produce nothing");
        CHECK(ducky_parse_line(&st, "CTRL ALT DELETE", a, 8) == 0,
              "not even a chord escapes the block");
        CHECK(ducky_parse_line(&st, "END_REM", a, 8) == 0, "block closes");
        /* "now it types" is 12 characters, so 12 key actions. */
        CHECK(ducky_parse_line(&st, "STRING now it types", a, 32) == 12,
              "and normal parsing resumes");
    }

    SUITE("DuckyScript: HOLD / RELEASE / RESET");
    {
        ducky_state_t st; ducky_state_init(&st);
        int n = ducky_parse_line(&st, "HOLD CTRL", a, 8);
        CHECK(n == 1 && a[0].kind == DUCKY_HOLD && a[0].mods == HID_MOD_LCTRL,
              "HOLD CTRL keeps ctrl down");
        n = ducky_parse_line(&st, "HOLD CTRL-SHIFT", a, 8);
        CHECK(a[0].mods == (HID_MOD_LCTRL|HID_MOD_LSHIFT), "HOLD takes hyphenated modifiers");
        n = ducky_parse_line(&st, "RELEASE", a, 8);
        CHECK(n == 1 && a[0].kind == DUCKY_RELEASE, "RELEASE lets go");
        n = ducky_parse_line(&st, "RESET", a, 8);
        CHECK(n == 1 && a[0].kind == DUCKY_RELEASE, "RESET clears held keys too");
    }

    SUITE("DuckyScript: WAIT_FOR_* lock-key conditions");
    {
        ducky_state_t st; ducky_state_init(&st);
        int n = ducky_parse_line(&st, "WAIT_FOR_CAPS_ON", a, 8);
        CHECK(n == 1 && a[0].kind == DUCKY_WAIT, "produces a wait action");
        CHECK(a[0].wait_mask == HID_LED_CAPSLOCK && a[0].wait_want == 1, "caps, on");
        ducky_parse_line(&st, "WAIT_FOR_NUM_OFF", a, 8);
        CHECK(a[0].wait_mask == HID_LED_NUMLOCK && a[0].wait_want == 0, "num, off");
        ducky_parse_line(&st, "WAIT_FOR_SCROLL_CHANGE", a, 8);
        CHECK(a[0].wait_mask == HID_LED_SCROLL && a[0].wait_want == 2, "scroll, change");
    }

    SUITE("DuckyScript: RANDOM_* types one character of the right class");
    {
        ducky_state_t st; ducky_state_init(&st);
        for (int i = 0; i < 40; i++) {
            int n = ducky_parse_line(&st, "RANDOM_NUMBER", a, 8);
            if (n != 1 || a[0].key < HID_KEY_1 || a[0].key > HID_KEY_0) {
                CHECK(false, "RANDOM_NUMBER produced a non-digit on iteration %d", i);
                break;
            }
        }
        CHECK(true, "40 random digits were all digits");
        int n = ducky_parse_line(&st, "RANDOM_LOWERCASE_LETTER", a, 8);
        CHECK(n == 1 && (a[0].mods & HID_MOD_LSHIFT) == 0, "lowercase is unshifted");
        n = ducky_parse_line(&st, "RANDOM_UPPERCASE_LETTER", a, 8);
        CHECK(n == 1 && (a[0].mods & HID_MOD_LSHIFT) != 0, "uppercase is shifted");
        CHECK(ducky_parse_line(&st, "RANDOM_CHAR", a, 8) == 1, "RANDOM_CHAR types something");
    }

    SUITE("DuckyScript: extra key names and mac aliases");
    {
        ducky_state_t st; ducky_state_init(&st);
        uint8_t k = 0;
        CHECK(hid_named_key("NUMLOCK", &k) && k == HID_KEY_NUMLOCK, "NUMLOCK");
        CHECK(hid_named_key("SCROLLLOCK", &k) && k == HID_KEY_SCROLLLOCK, "SCROLLLOCK");
        CHECK(hid_named_key("PAUSE", &k) && k == HID_KEY_PAUSE, "PAUSE");
        CHECK(hid_named_key("BREAK", &k) && k == HID_KEY_PAUSE, "BREAK is PAUSE");
        uint8_t m = 0;
        CHECK(hid_modifier("OPTION", &m) && m == HID_MOD_LALT, "OPTION is ALT (mac)");
        CHECK(hid_modifier("CMD", &m) && m == HID_MOD_LGUI, "CMD is GUI (mac)");
        CHECK(hid_modifier("COMMAND", &m) && m == HID_MOD_LGUI, "COMMAND is GUI");
    }

    SUITE("chords: a shifted symbol keeps the shift that produces it");
    {
        ducky_state_t st; ducky_state_init(&st);
        ducky_action_t a[8];
        /* US '+' is Shift+'='. Dropping the shift pressed '=' instead. */
        int n = ducky_parse_line(&st, "CTRL +", a, 8);
        CHECK(n == 1 && a[0].kind == DUCKY_KEY, "CTRL + parses");
        CHECK((a[0].mods & HID_MOD_LCTRL) != 0, "ctrl is held");
        CHECK((a[0].mods & HID_MOD_LSHIFT) != 0, "and so is the shift '+' needs");

        /* ...but a letter must NOT gain one: CTRL A is Ctrl+A. */
        n = ducky_parse_line(&st, "CTRL A", a, 8);
        CHECK(n == 1 && (a[0].mods & HID_MOD_LSHIFT) == 0,
              "CTRL A stays Ctrl+A, not Ctrl+Shift+A");
        n = ducky_parse_line(&st, "CTRL a", a, 8);
        CHECK(n == 1 && (a[0].mods & HID_MOD_LSHIFT) == 0, "and so does lowercase");
    }

    SUITE("keys: F13-F24 are real usages, not errors");
    {
        ducky_state_t st; ducky_state_init(&st);
        ducky_action_t a[8];
        uint8_t k = 0;
        CHECK(hid_named_key("F13", &k) && k == 0x68, "F13 = 0x68, got 0x%02X", k);
        CHECK(hid_named_key("F24", &k) && k == 0x73, "F24 = 0x73, got 0x%02X", k);
        CHECK(!hid_named_key("F25", &k), "F25 does not exist");
        CHECK(ducky_parse_line(&st, "CTRL F13", a, 8) == 1, "CTRL F13 is a valid chord");
    }

    SUITE("HOLD carries a normal key, not just modifiers");
    {
        ducky_state_t st; ducky_state_init(&st);
        ducky_action_t a[8];
        int n = ducky_parse_line(&st, "HOLD SPACE", a, 8);
        CHECK(n == 1 && a[0].kind == DUCKY_HOLD, "HOLD SPACE parses to a hold");
        CHECK(a[0].key == HID_KEY_SPACE, "and names the key the player must press");
    }

    SUITE("STRING: text longer than the action buffer is CONTINUED, not cut off");
    {
        /* 400 characters into a 192-action buffer used to type 192 of them and
         * drop the rest in silence. */
        ducky_state_t st; ducky_state_init(&st);
        static ducky_action_t a[192];
        char line[600]; int w = 0;
        w += sprintf(line, "STRING ");
        for (int i = 0; i < 400; i++) line[w++] = (char)('A' + (i % 26));
        line[w] = 0;

        int typed = 0, passes = 0;
        int k = ducky_parse_line(&st, line, a, 192);
        for (int i = 0; i < k; i++) if (a[i].kind == DUCKY_KEY) typed++;
        while (st.pending && passes < 20) {
            k = ducky_continue(&st, a, 192);
            if (k <= 0) break;
            for (int i = 0; i < k; i++) if (a[i].kind == DUCKY_KEY) typed++;
            passes++;
        }
        CHECK(typed == 400, "all 400 characters are typed, got %d", typed);
        CHECK(st.pending == NULL, "and nothing is left pending");

        /* the letters must come out in order across the chunk boundary */
        ducky_state_init(&st);
        k = ducky_parse_line(&st, line, a, 192);
        uint8_t first_key = a[0].key, boundary = a[k - 1].key;
        k = ducky_continue(&st, a, 192);
        CHECK(k > 0, "a second chunk follows");
        CHECK(a[0].key != boundary,
              "it resumes AFTER the character the first chunk ended on");
        CHECK(a[0].key != first_key, "and does not restart from the beginning");
    }

    SUITE("STRINGLN: the newline waits for the end of a long line");
    {
        ducky_state_t st; ducky_state_init(&st);
        static ducky_action_t a[192];
        char line[600]; int w = sprintf(line, "STRINGLN ");
        for (int i = 0; i < 400; i++) line[w++] = 'x';
        line[w] = 0;
        int k = ducky_parse_line(&st, line, a, 192);
        bool early = false;
        for (int i = 0; i < k; i++) if (a[i].key == HID_KEY_ENTER) early = true;
        CHECK(!early, "no ENTER in the first chunk");
        int enters = 0;
        while (st.pending) {
            k = ducky_continue(&st, a, 192);
            if (k <= 0) break;
            for (int i = 0; i < k; i++) if (a[i].key == HID_KEY_ENTER) enters++;
        }
        CHECK(enters == 1, "exactly one ENTER, at the very end (got %d)", enters);
    }

    SUITE("media keys accept the spellings payloads actually use");
    {
        ducky_state_t st; ducky_state_init(&st);
        ducky_action_t a[8];
        /* DuckyScript 3 writes the underscored form; it was rejected outright. */
        CHECK(ducky_parse_line(&st, "MEDIA VOLUME_UP", a, 8) == 1 &&
              a[0].kind == DUCKY_CONSUMER && a[0].consumer == 0xE9, "MEDIA VOLUME_UP");
        CHECK(ducky_parse_line(&st, "MEDIA VOLUME_DOWN", a, 8) == 1 &&
              a[0].consumer == 0xEA, "MEDIA VOLUME_DOWN");
        CHECK(ducky_parse_line(&st, "MEDIA PLAY_PAUSE", a, 8) == 1 &&
              a[0].consumer == 0xCD, "MEDIA PLAY_PAUSE");
        /* the old spellings must keep working */
        CHECK(ducky_parse_line(&st, "MEDIA VOLUMEUP", a, 8) == 1 &&
              a[0].consumer == 0xE9, "MEDIA VOLUMEUP still works");
        /* and the standalone command form */
        CHECK(ducky_parse_line(&st, "MEDIA_VOLUME_UP", a, 8) == 1 &&
              a[0].kind == DUCKY_CONSUMER && a[0].consumer == 0xE9, "MEDIA_VOLUME_UP alone");
        CHECK(ducky_parse_line(&st, "MEDIA_MUTE", a, 8) == 1 &&
              a[0].consumer == 0xE2, "MEDIA_MUTE alone");
        CHECK(ducky_parse_line(&st, "MEDIA BANANA", a, 8) == 0, "an unknown media key is still rejected");
    }
TEST_MAIN_END
