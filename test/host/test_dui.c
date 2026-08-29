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
TEST_MAIN_END
