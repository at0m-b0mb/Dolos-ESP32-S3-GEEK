#include "dui.h"
#include "dconfig.h"
#include "bmp.h"
#include <string.h>
#include <stdio.h>
static uint16_t buf[240 * 135];
static void save(const char *dir, const char *name, const dui_state_t *st, int splash)
{
    canvas_t cv; cv_init(&cv, buf, 240, 135);
    if (splash) dui_render_splash(&cv); else dui_render(&cv, st);
    char path[512]; snprintf(path, sizeof(path), "%s/%s", dir, name);
    bmp_write(path, &cv, 3);
    printf("  %s (oob=%u)\n", name, cv.oob);
}
int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    dui_state_t st; memset(&st, 0, sizeof(st));
    st.payload_name = "PAYLOAD.TXT"; st.total_lines = 14; st.usb_mounted = true;
    save(dir, "dui_splash.bmp", &st, 1);
    st.mode = DUI_SAFE;                      save(dir, "dui_safe.bmp", &st, 0);
    st.mode = DUI_ARMED;                     save(dir, "dui_armed.bmp", &st, 0);
    st.mode = DUI_COUNTDOWN; st.countdown=2; save(dir, "dui_countdown.bmp", &st, 0);
    st.mode = DUI_RUNNING; st.cur_line = 8;  save(dir, "dui_running.bmp", &st, 0);
    st.mode = DUI_DONE;                      save(dir, "dui_done.bmp", &st, 0);

    /* settings menu */
    dolos_config_t mcfg; config_defaults(&mcfg); mcfg.wifi_on = true;
    st.mode = DUI_MENU; st.menu_sel = 2; st.cfg = &mcfg;
    save(dir, "dui_menu.bmp", &st, 0);

    /* console info: QR + credentials */
    st.mode = DUI_INFO; st.wifi_on = true; st.console_up = true;
    st.wifi_ssid = "Dolos-7C21"; st.wifi_key = "CG6892MXQ3JHTN4P";
    st.admin_user = "admin"; st.admin_pw = "5NFVH6RQ2WKMDX";
    save(dir, "dui_console.bmp", &st, 0);
    return 0;
}
