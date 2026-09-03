#include "dolos_test.h"
#include "dconfig.h"
#include <string.h>

TEST_MAIN_BEGIN
    SUITE("config: defaults are safe");
    {
        dolos_config_t c; config_defaults(&c);
        CHECK(c.layout == LAYOUT_US, "default layout US");
        CHECK(c.speed == SPEED_BALANCED, "default speed balanced");
        CHECK(!c.dry_run, "dry-run off by default");
        CHECK(c.arm_pin[0] == 0, "no PIN by default");
    }

    SUITE("config: parses keys, ignores comments, trims");
    {
        dolos_config_t c; config_defaults(&c);
        config_parse("# Dolos config\n"
                     "layout = de\n"
                     "speed=fast\n"
                     "dryrun = on\n"
                     "defaultdelay=25\n"
                     "armpin=2 3 1x\n", &c);
        CHECK(c.layout == LAYOUT_DE, "layout de");
        CHECK(c.speed == SPEED_FAST, "speed fast");
        CHECK(c.dry_run, "dry-run on");
        CHECK(c.default_delay_ms == 25, "default delay 25");
        CHECK(strcmp(c.arm_pin, "231") == 0, "pin keeps only digits 1-9, got '%s'", c.arm_pin);
    }

    SUITE("config: speed profiles map to sane delays");
    {
        CHECK(speed_key_delay_ms(SPEED_FAST) < speed_key_delay_ms(SPEED_BALANCED), "fast < balanced");
        CHECK(speed_key_delay_ms(SPEED_BALANCED) < speed_key_delay_ms(SPEED_RELIABLE), "balanced < reliable");
    }

    SUITE("config: ui_lock is a level, and 'on' still means what it always did");
    {
        dolos_config_t c;
        config_defaults(&c);
        CHECK(c.ui_lock == UI_LOCK_OFF, "unlocked by default");
        config_parse("ui_lock=on\n", &c);
        CHECK(c.ui_lock == UI_LOCK_MENU, "'on' locks the menu (backward compatible)");
        config_defaults(&c); config_parse("ui_lock=menu\n", &c);
        CHECK(c.ui_lock == UI_LOCK_MENU, "'menu' locks the menu");
        config_defaults(&c); config_parse("ui_lock=full\n", &c);
        CHECK(c.ui_lock == UI_LOCK_FULL, "'full' locks everything");
        config_defaults(&c); config_parse("ui_lock=off\n", &c);
        CHECK(c.ui_lock == UI_LOCK_OFF, "'off' unlocks");
        config_defaults(&c); config_parse("ui_lock=banana\n", &c);
        CHECK(c.ui_lock == UI_LOCK_OFF, "an unknown value fails open, not into a stuck lock");
        /* the levels must stay ordered: main compares with < and == */
        CHECK(UI_LOCK_OFF < UI_LOCK_MENU && UI_LOCK_MENU < UI_LOCK_FULL,
              "levels are ordered off < menu < full");
    }

    SUITE("config: an over-long line cannot invent a second setting");
    {
        /* The parser used to resume mid-line, so the tail of a long value was
         * read as a new key=value pair. */
        dolos_config_t c; config_defaults(&c);
        char big[400];
        int n = snprintf(big, sizeof(big), "uplink_pass=");
        for (int i = 0; i < 200; i++) big[n++] = 'x';
        n += snprintf(big + n, sizeof(big) - n, "\nlayout=de\n");
        big[n] = 0;
        config_parse(big, &c);
        CHECK(c.layout == LAYOUT_DE,
              "the line AFTER the long one is still parsed (got %s)", layout_name(c.layout));

        /* and the tail must not have been read as its own setting */
        dolos_config_t c2; config_defaults(&c2);
        char evil[400];
        int m = snprintf(evil, sizeof(evil), "wifi_ssid=");
        for (int i = 0; i < 130; i++) evil[m++] = 'a';
        m += snprintf(evil + m, sizeof(evil) - m, "speed=fast\n");
        evil[m] = 0;
        config_parse(evil, &c2);
        CHECK(c2.speed == SPEED_BALANCED,
              "the spliced tail did NOT set speed (got %s)", speed_name(c2.speed));
    }

    SUITE("config: default delay is clamped");
    {
        dolos_config_t c; config_defaults(&c);
        config_parse("defaultdelay=999999999\n", &c);
        CHECK(c.default_delay_ms <= 60000, "clamped, got %lu", (unsigned long)c.default_delay_ms);
        config_parse("defaultdelay=-5\n", &c);
        CHECK(c.default_delay_ms == 0, "negative becomes zero, got %lu", (unsigned long)c.default_delay_ms);
        config_parse("defaultdelay=250\n", &c);
        CHECK(c.default_delay_ms == 250, "a sane value is kept");
    }

TEST_MAIN_END
