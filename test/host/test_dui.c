#include "dolos_test.h"
#include "dui.h"
static uint16_t buf[240 * 135];
TEST_MAIN_BEGIN
    canvas_t cv; cv_init(&cv, buf, 240, 135);
    dui_state_t st; memset(&st, 0, sizeof(st));
    st.payload_name = "PAYLOAD.TXT"; st.total_lines = 12;

    SUITE("dui: every mode fits the 240x135 panel (no off-screen draw)");
    {
        dui_mode_t modes[] = { DUI_SAFE, DUI_ARMED, DUI_COUNTDOWN, DUI_RUNNING, DUI_DONE };
        for (int i = 0; i < 5; i++) {
            st.mode = modes[i]; st.countdown = 3; st.cur_line = 7; st.usb_mounted = (i % 2);
            cv.oob = 0;
            dui_render(&cv, &st);
            CHECK(cv.oob == 0, "mode %d must not draw off-panel, oob=%u", modes[i], cv.oob);
        }
        cv.oob = 0; dui_render_splash(&cv);
        CHECK(cv.oob == 0, "splash must not draw off-panel, oob=%u", cv.oob);
    }

    SUITE("dui: long payload names + full counts still fit");
    {
        st.mode = DUI_RUNNING; st.payload_name = "A_VERY_LONG_PAYLOAD_NAME.TXT";
        st.total_lines = 9999; st.cur_line = 9999; st.usb_mounted = true;
        cv.oob = 0; dui_render(&cv, &st);
        CHECK(cv.oob == 0, "saturated running screen must fit, oob=%u", cv.oob);
    }

    SUITE("dui: the settings menu fits, on every row and with the longest values");
    {
        dolos_config_t cfg; config_defaults(&cfg);
        cfg.wifi_on = true; cfg.remote_fire = true; cfg.dry_run = true;
        st.mode = DUI_MENU; st.cfg = &cfg;
        for (int sel = 0; sel < MENU__COUNT; sel++) {
            st.menu_sel = sel;
            /* widest possible values on every row at once */
            cfg.layout = LAYOUT_LATAM; cfg.os = OS_WINDOWS; cfg.speed = SPEED_RELIABLE;
            cv.oob = 0;
            dui_render(&cv, &st);
            CHECK_QUIET(cv.oob == 0, "menu row %d drew off-panel, oob=%u", sel, cv.oob);
        }
        CHECK(1, "all %d menu rows fit the panel", MENU__COUNT);

        /* every layout/os/speed combination must fit too */
        int bad = 0;
        for (int l = 0; l < LAYOUT__COUNT; l++)
            for (int o = 0; o < 3; o++)
                for (int sp = 0; sp < 3; sp++) {
                    cfg.layout = (kb_layout_t)l; cfg.os = (target_os_t)o;
                    cfg.speed = (dolos_speed_t)sp;
                    cv.oob = 0; dui_render(&cv, &st);
                    if (cv.oob) bad++;
                }
        CHECK(bad == 0, "%d layout/os/speed combinations overflowed", bad);

        /* a NULL config must not crash the renderer */
        st.cfg = NULL; cv.oob = 0; dui_render(&cv, &st);
        CHECK(cv.oob == 0, "menu with no config still renders cleanly");
        st.cfg = &cfg;
    }

    SUITE("dui: SAFE screen with the settings hint and with the UI locked");
    {
        st.mode = DUI_SAFE; st.lint_problems = 0; st.wifi_on = true;
        st.wifi_ssid = "Dolos-AB12"; st.admin_pw = "A1B2C3D4";
        ui_lock_t levels[] = { UI_LOCK_OFF, UI_LOCK_MENU, UI_LOCK_FULL };
        for (int i = 0; i < 3; i++) {
            st.ui_lock = levels[i];
            st.wifi_on = true;  cv.oob = 0; dui_render(&cv, &st);
            CHECK_QUIET(cv.oob == 0, "lock level %d with AP overflowed", levels[i]);
            st.wifi_on = false; cv.oob = 0; dui_render(&cv, &st);
            CHECK_QUIET(cv.oob == 0, "lock level %d without AP overflowed", levels[i]);
        }
        CHECK(1, "every lock level fits, with and without the AP lines");
        /* the LOCK badge must not collide with the badges that outrank it */
        st.ui_lock = UI_LOCK_FULL; st.dry_run = true; st.remote_fire_enabled = true;
        cv.oob = 0; dui_render(&cv, &st);
        CHECK(cv.oob == 0, "lock + dry-run + remote-fire together fit, oob=%u", cv.oob);
        st.dry_run = false; st.remote_fire_enabled = false; st.ui_lock = UI_LOCK_OFF;
        st.wifi_on = true;
    }

    SUITE("dui: a payload with lint errors shows the block, not the arm hint");
    {
        st.mode = DUI_SAFE; st.lint_problems = 2; st.lint_line = 7;
        st.lint_msg = "text has a character it cannot type";
        cv.oob = 0; dui_render(&cv, &st);
        CHECK(cv.oob == 0, "lint-error SAFE screen fits, oob=%u", cv.oob);
        st.lint_problems = 0; st.lint_msg = NULL;
    }

    SUITE("dui: the standing AUTHORIZED USE ONLY tag fits beside the LED indicators");
    {
        /* The footer packs three things onto one 240 px row: layout/speed on the
         * left, the Caps/Num/Scroll indicators in the middle, and the standing
         * reminder right-aligned. Widening that reminder is exactly the change
         * that would silently collide with the indicators, so the geometry is
         * pinned here rather than eyeballed. */
        const int led_right_edge = 96 + 16 + cv_text_width("S", 1);   /* last LED glyph */
        const int tag_left = 240 - cv_text_width("AUTHORIZED USE ONLY", 1) - 3;
        CHECK(tag_left > led_right_edge,
              "tag starts at x=%d, LEDs end at x=%d - they must not overlap",
              tag_left, led_right_edge);
        CHECK(tag_left >= 0, "tag stays on the panel, x=%d", tag_left);

        /* and it must render clean in the worst case: longest layout/speed on
         * the left with every LED lit */
        st.mode = DUI_SAFE; st.layout = "US"; st.speed = "BALANCED"; st.leds = 0x07;
        cv.oob = 0; dui_render(&cv, &st);
        CHECK(cv.oob == 0, "footer with all LEDs lit fits, oob=%u", cv.oob);
        st.leds = 0;
    }

    SUITE("dui: the console screen fits, with a QR and full-length credentials");
    {
        dolos_config_t cfg2; config_defaults(&cfg2); cfg2.wifi_on = true;
        st.mode = DUI_INFO; st.cfg = &cfg2; st.wifi_on = true;
        st.wifi_ssid = "Dolos-4F2A"; st.wifi_key = "K7QM4XR2TB";
        st.admin_user = "admin"; st.admin_pw = "P4XK9WDT";
        cv.oob = 0; dui_render(&cv, &st);
        CHECK(cv.oob == 0, "console screen with QR fits the panel, oob=%u", cv.oob);

        /* longest realistic strings: a 31-char SSID and full-width secrets */
        st.wifi_ssid = "Dolos-AAAAAAAAAAAAAAAAAAAAAAAAA";
        st.wifi_key  = "ABCDEFGHIJKLMNOPQRST";
        st.admin_user = "administrator"; st.admin_pw = "ABCDEFGHIJKL";
        cv.oob = 0; dui_render(&cv, &st);
        CHECK(cv.oob == 0, "console screen with maximal strings fits, oob=%u", cv.oob);

        /* radio off: the screen must explain itself, not show a blank QR */
        st.wifi_on = false; cv.oob = 0; dui_render(&cv, &st);
        CHECK(cv.oob == 0, "console screen with the radio off fits, oob=%u", cv.oob);

        /* missing strings must not crash the QR encoder */
        st.wifi_on = true; st.wifi_ssid = NULL; st.wifi_key = NULL;
        st.admin_user = NULL; st.admin_pw = NULL;
        cv.oob = 0; dui_render(&cv, &st);
        CHECK(cv.oob == 0, "console screen with no credentials still renders, oob=%u", cv.oob);
    }

    SUITE("dui: the console screen rows do not collide when the password is hidden");
    {
        dolos_config_t c4; config_defaults(&c4); c4.wifi_on = true;
        st.mode = DUI_INFO; st.cfg = &c4; st.wifi_on = true;
        st.wifi_ssid = "Dolos-7C21"; st.wifi_key = "7HHLUYVW2NXKUCWL";
        st.admin_user = "admin"; st.admin_pw = "CA86H6W559ZTPR";

        /* masked: the hint used to be drawn on the same row as the URL, which
         * produced "HOLD:/:REVEAL8.4.1" on the real device */
        st.admin_pw_masked = true;
        cv.oob = 0; dui_render(&cv, &st);
        CHECK(cv.oob == 0, "masked console screen fits, oob=%u", cv.oob);

        st.admin_pw_masked = false;
        cv.oob = 0; dui_render(&cv, &st);
        CHECK(cv.oob == 0, "revealed console screen fits, oob=%u", cv.oob);
    }

    SUITE("font: every character the UI actually draws has a glyph");
    {
        /* The password mask was drawn with '*', which had no glyph in the large
         * face and therefore rendered as blank - the screen simply showed
         * nothing where the password should be. */
        const char *used = "*.:/@_-";
        int missing = 0;
        for (const char *p = used; *p; p++) {
            int idx = (*p - FONT7X12_FIRST) * FONT7X12_H;
            int on = 0;
            for (int r = 0; r < FONT7X12_H; r++) if (aegis_font7x12[idx + r]) on = 1;
            if (!on) { missing++; }
        }
        CHECK(missing == 0, "%d symbol(s) the UI uses have no glyph", missing);
    }

    SUITE("dui: the restart notice fits, including long text");
    {
        cv.oob = 0;
        dui_render_notice(&cv, "NEW CREDENTIALS", "RESTARTING - REJOIN WITH", "THE NEW KEY ON SCREEN");
        CHECK(cv.oob == 0, "new-credentials notice fits, oob=%u", cv.oob);
        cv.oob = 0;
        dui_render_notice(&cv, "FACTORY RESET", "ERASING SETTINGS AND", "CREDENTIALS - RESTARTING");
        CHECK(cv.oob == 0, "factory-reset notice fits, oob=%u", cv.oob);
        cv.oob = 0;
        dui_render_notice(&cv, "X", NULL, NULL);
        CHECK(cv.oob == 0, "a notice with no body still renders");
    }

    SUITE("dui: the console screen says WHY the radio is off");
    {
        dolos_config_t c5; config_defaults(&c5);
        st.mode = DUI_INFO; st.cfg = &c5; st.wifi_on = false;
        st.safe_boot = false; st.degraded = false;

        /* genuinely switched off - the advice is correct */
        c5.wifi_on = false;
        cv.oob = 0; dui_render(&cv, &st);
        CHECK(cv.oob == 0, "off-in-settings screen fits");

        /* switched ON but not running: telling the operator to enable it in
         * settings would send them to a switch that is already on */
        c5.wifi_on = true;
        cv.oob = 0; dui_render(&cv, &st);
        CHECK(cv.oob == 0, "on-but-not-running screen fits");

        /* and after a crash the reason is the crash, not the setting */
        st.safe_boot = true;
        cv.oob = 0; dui_render(&cv, &st);
        CHECK(cv.oob == 0, "safe-boot screen fits");
        st.safe_boot = false;
    }

    SUITE("dui: a run that typed nothing says so, instead of looking like a success");
    {
        dolos_config_t c9; config_defaults(&c9);
        memset(&st, 0, sizeof(st)); st.cfg = &c9;
        st.mode = DUI_DONE; st.payload_name = "PAYLOAD.TXT";
        st.run_failed = true; st.run_fail_msg = "OUT OF MEMORY FOR THE INTERPRETER";
        cv.oob = 0; dui_render(&cv, &st);
        CHECK(cv.oob == 0, "the failure screen fits, oob=%u", cv.oob);

        st.run_failed = false; st.run_fail_msg = NULL;
        cv.oob = 0; dui_render(&cv, &st);
        CHECK(cv.oob == 0, "the ordinary SENT screen still fits, oob=%u", cv.oob);
    }

    SUITE("canvas: an absent string draws nothing instead of crashing");
    {
        static uint16_t px[240 * 135];
        canvas_t cv; cv_init(&cv, px, 240, 135);
        cv_clear(&cv, 0);
        int x = cv_text(&cv, 10, 10, NULL, 0xFFFF, -1, 1);
        CHECK(x == 10, "NULL text advances the cursor not at all");
        CHECK(cv_text_width(NULL, 1) == 0, "and measures as zero wide");
        cv_text_center(&cv, 120, 20, NULL, 0xFFFF, -1, 1);
        CHECK(cv.oob == 0, "nothing was drawn out of bounds");
    }
TEST_MAIN_END
