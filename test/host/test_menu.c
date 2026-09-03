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
        /* AUTO -> WINDOWS -> LINUX -> MAC -> AUTO: four states, not three,
         * because automatic detection is a choice you can select. */
        CHECK(c.os_auto, "the default is to detect the OS, not to assume one");
        menu_activate(&c, MENU_OS); CHECK(!c.os_auto && c.os == OS_WINDOWS, "auto -> windows");
        menu_activate(&c, MENU_OS); CHECK(c.os == OS_LINUX, "windows -> linux");
        menu_activate(&c, MENU_OS); CHECK(c.os == OS_MAC,   "linux -> mac");
        menu_activate(&c, MENU_OS); CHECK(c.os_auto, "mac wraps back to auto");
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
        c.layout = LAYOUT_FR; c.os = OS_MAC; c.os_auto = false; c.speed = SPEED_FAST;
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
        CHECK(back.os == OS_MAC && !back.os_auto, "a manually chosen os survived");
        CHECK(back.speed == SPEED_FAST, "speed survived");
        CHECK(back.dry_run == true, "dry-run survived");
        CHECK(back.default_delay_ms == 77, "default delay survived");
        CHECK(back.remote_fire == true, "remote-fire survived");
        CHECK(back.wifi_on == true, "wifi survived");
        CHECK(back.ui_lock == UI_LOCK_FULL, "ui_lock level survived, got %d", back.ui_lock);
        CHECK(strcmp(back.arm_pin, "1234") == 0, "arm pin survived, got '%s'", back.arm_pin);
        CHECK(strcmp(back.wifi_ssid, "Dolos-TEST") == 0, "ssid survived, got '%s'", back.wifi_ssid);
        CHECK(strcmp(back.admin_user, "root") == 0, "admin user survived");
        /* Secrets deliberately do NOT survive: they are never written to the
         * card. This assertion used to require the opposite - it encoded the
         * behaviour that put the Wi-Fi key and console password in plaintext on
         * removable media, so it is inverted here on purpose. */
        CHECK(back.wifi_pass[0] == 0, "wifi key is NOT written to the card");
        CHECK(back.admin_pass[0] == 0, "console password is NOT written to the card");
    }

    SUITE("config: a buffer that is too small fails loudly rather than truncating");
    {
        dolos_config_t c; config_defaults(&c);
        char tiny[16];
        CHECK(config_write_text(&c, tiny, sizeof(tiny)) == 0, "reports failure, writes no half-config");
        CHECK(config_write_text(&c, tiny, 0) == 0, "zero capacity is handled");
    }

    SUITE("config: secrets are NEVER written to the SD card");
    {
        dolos_config_t c; config_defaults(&c);
        strcpy(c.wifi_ssid,  "Dolos-7C21");
        strcpy(c.wifi_pass,  "SUPERSECRETWIFIKEY");
        strcpy(c.admin_user, "admin");
        strcpy(c.admin_pass, "SUPERSECRETADMINPW");
        char out[1024];
        size_t n = config_write_text(&c, out, sizeof(out));
        CHECK(n > 0, "config serialises");
        /* An SD card is removable and readable on any laptop. Whatever else
         * this file contains, it must not contain these. */
        CHECK(strstr(out, "SUPERSECRETWIFIKEY") == NULL,
              "the Wi-Fi key must not appear in DOLOS.CFG");
        CHECK(strstr(out, "SUPERSECRETADMINPW") == NULL,
              "the console password must not appear in DOLOS.CFG");
        /* non-secret settings still round-trip */
        CHECK(strstr(out, "wifi_ssid=Dolos-7C21") != NULL, "the SSID is still saved");
        CHECK(strstr(out, "admin_user=admin") != NULL, "the username is still saved");

        dolos_config_t back; config_defaults(&back);
        config_parse(out, &back);
        CHECK(back.layout == c.layout && back.speed == c.speed,
              "settings survive the round trip");
        CHECK(back.wifi_pass[0] == 0,
              "a reloaded file carries no key - the device uses the one in NVS");
    }

    SUITE("config: every parsed setting is also WRITTEN back");
    {
        /* Settings the writer forgot were silently lost at the next boot. */
        dolos_config_t c; config_defaults(&c);
        c.sta_on = true;
        snprintf(c.sta_ssid, sizeof(c.sta_ssid), "HomeNet");
        c.bootlog = true;
        c.msc_enabled = true;
        c.msc_partition = 3;
        c.ui_lock = UI_LOCK_MENU;

        char text[1024];
        size_t n = config_write_text(&c, text, sizeof(text));
        CHECK(n > 0, "the file was written");

        dolos_config_t back; config_defaults(&back);
        config_parse(text, &back);
        CHECK(back.sta_on == c.sta_on, "uplink survives the round trip");
        CHECK(strcmp(back.sta_ssid, c.sta_ssid) == 0, "uplink SSID survives, got '%s'", back.sta_ssid);
        CHECK(back.bootlog == c.bootlog, "boot log survives");
        CHECK(back.msc_enabled == c.msc_enabled, "storage sharing survives");
        CHECK(back.msc_partition == c.msc_partition,
              "storage partition survives, got %u", back.msc_partition);
        CHECK(back.ui_lock == c.ui_lock, "ui lock survives");
    }

    SUITE("os=auto round-trips as the word auto");
    {
        dolos_config_t c; config_defaults(&c);
        CHECK(c.os_auto, "auto is the default");
        char text[1024];
        size_t n = config_write_text(&c, text, sizeof(text));
        CHECK(n > 0 && strstr(text, "os=auto") != NULL, "written as os=auto");

        dolos_config_t back; config_defaults(&back);
        back.os_auto = false; back.os = OS_LINUX;
        config_parse(text, &back);
        CHECK(back.os_auto, "and read back as automatic");

        /* an explicit setting still wins, and still turns automatic off */
        config_parse("os=mac\n", &back);
        CHECK(!back.os_auto && back.os == OS_MAC, "os=mac overrides detection");
        config_parse("os=detect\n", &back);
        CHECK(back.os_auto, "os=detect is accepted as a spelling of auto");
    }
TEST_MAIN_END
