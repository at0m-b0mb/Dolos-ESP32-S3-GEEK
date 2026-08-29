#include "dolos_test.h"
#include "menu.h"
#include <string.h>

TEST_MAIN_BEGIN
    char v[32];

    SUITE("menu: every item has a label");
    {
        for (int i = 0; i < MENU__COUNT; i++) {
            const char *l = menu_label((menu_item_t)i);
            CHECK_QUIET(l && l[0] && strcmp(l, "-") != 0, "item %d has a real label", i);
        }
        CHECK(1, "all %d items are labelled", MENU__COUNT);
    }

    SUITE("menu: toggles flip");
    {
        dolos_config_t c; config_defaults(&c);
        bool before = c.dry_run;
        menu_activate(&c, MENU_DRYRUN);
        CHECK(c.dry_run != before, "dry-run toggles");
        menu_value(&c, MENU_DRYRUN, v, sizeof(v));
        CHECK(strcmp(v, c.dry_run ? "ON" : "OFF") == 0, "value string tracks the flag, got %s", v);
        menu_activate(&c, MENU_DRYRUN);
        CHECK(c.dry_run == before, "toggling twice returns to the start");
    }

    SUITE("menu: choices cycle through every option and wrap");
    {
        dolos_config_t c; config_defaults(&c);
        /* layout must visit all LAYOUT__COUNT values and come back */
        kb_layout_t first = c.layout;
        for (int i = 0; i < LAYOUT__COUNT; i++) menu_activate(&c, MENU_LAYOUT);
        CHECK(c.layout == first, "layout wraps after a full cycle");
        for (int i = 0; i < 3; i++) menu_activate(&c, MENU_OS);
        CHECK(c.os == OS_WINDOWS, "os wraps after 3 steps");
        for (int i = 0; i < 3; i++) menu_activate(&c, MENU_SPEED);
        CHECK(c.speed == SPEED_BALANCED, "speed wraps after 3 steps");
        /* no cycle may ever land out of range */
        for (int i = 0; i < 50; i++) {
            menu_activate(&c, MENU_LAYOUT); menu_activate(&c, MENU_OS); menu_activate(&c, MENU_SPEED);
            CHECK_QUIET(c.layout < LAYOUT__COUNT && c.os <= OS_MAC && c.speed <= SPEED_RELIABLE,
                        "values stayed in range");
        }
        CHECK(1, "50 cycles stayed in range");
    }

    SUITE("menu: save and exit report an action instead of changing settings");
    {
        dolos_config_t c; config_defaults(&c);
        dolos_config_t before = c;
        CHECK(menu_activate(&c, MENU_SAVE) == MENU_ACT_SAVE, "SAVE asks the caller to save");
        CHECK(menu_activate(&c, MENU_EXIT) == MENU_ACT_EXIT, "EXIT asks the caller to close");
        CHECK(memcmp(&before, &c, sizeof(c)) == 0, "neither one modified the config");
        CHECK(menu_activate(&c, MENU_DRYRUN) == MENU_ACT_NONE, "a normal toggle asks for nothing");
    }

    SUITE("menu: wifi is flagged as needing a restart, others are not");
    {
        CHECK(menu_needs_reboot(MENU_WIFI), "wifi needs a restart");
        CHECK(!menu_needs_reboot(MENU_LAYOUT), "layout applies live");
        CHECK(!menu_needs_reboot(MENU_DRYRUN), "dry-run applies live");
    }

    SUITE("config: saved settings round-trip through the parser");
    {
        dolos_config_t c; config_defaults(&c);
        c.layout = LAYOUT_FR; c.os = OS_MAC; c.speed = SPEED_FAST;
        c.dry_run = true; c.default_delay_ms = 77; c.remote_fire = true; c.wifi_on = true;
        c.ui_lock = UI_LOCK_FULL;
        strcpy(c.arm_pin, "1234");
        strcpy(c.wifi_ssid, "Dolos-TEST");
        strcpy(c.wifi_pass, "hunter2hunter2");
        strcpy(c.admin_user, "root");
        strcpy(c.admin_pass, "s3cret");

        char text[768];
        size_t n = config_write_text(&c, text, sizeof(text));
        CHECK(n > 0 && n < sizeof(text), "serialised into the buffer, %zu bytes", n);

        dolos_config_t back; config_defaults(&back);
        config_parse(text, &back);
        CHECK(back.layout == LAYOUT_FR, "layout survived");
        CHECK(back.os == OS_MAC, "os survived");
        CHECK(back.speed == SPEED_FAST, "speed survived");
        CHECK(back.dry_run == true, "dry-run survived");
        CHECK(back.default_delay_ms == 77, "default delay survived");
        CHECK(back.remote_fire == true, "remote-fire survived");
        CHECK(back.wifi_on == true, "wifi survived");
        CHECK(back.ui_lock == UI_LOCK_FULL, "ui_lock level survived, got %d", back.ui_lock);
        CHECK(strcmp(back.arm_pin, "1234") == 0, "arm pin survived, got '%s'", back.arm_pin);
        CHECK(strcmp(back.wifi_ssid, "Dolos-TEST") == 0, "ssid survived, got '%s'", back.wifi_ssid);
        CHECK(strcmp(back.wifi_pass, "hunter2hunter2") == 0, "wifi pass survived");
        CHECK(strcmp(back.admin_user, "root") == 0, "admin user survived");
        CHECK(strcmp(back.admin_pass, "s3cret") == 0, "admin pass survived");
    }

    SUITE("config: a buffer that is too small fails loudly rather than truncating");
    {
        dolos_config_t c; config_defaults(&c);
        char tiny[16];
        CHECK(config_write_text(&c, tiny, sizeof(tiny)) == 0, "reports failure, writes no half-config");
        CHECK(config_write_text(&c, tiny, 0) == 0, "zero capacity is handled");
    }
TEST_MAIN_END
