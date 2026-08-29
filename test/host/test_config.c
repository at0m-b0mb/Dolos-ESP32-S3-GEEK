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
TEST_MAIN_END
