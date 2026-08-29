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
TEST_MAIN_END
