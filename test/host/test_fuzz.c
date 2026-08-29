/*
 * test_fuzz.c - throw hostile and malformed input at every parser.
 *
 * Dolos reads three things it does not control: a config file, payload text,
 * and (with the console on) HTTP form fields. All of them arrive from an SD
 * card someone else may have written or over the air. A parser that walks off
 * the end of a buffer on a truncated UTF-8 sequence is not a cosmetic bug on a
 * device whose whole job is to be trusted with a keyboard.
 *
 * Build this suite with ASan+UBSan (see the `fuzz` target) so overruns and
 * undefined behaviour abort the run instead of passing quietly.
 */
#include "dolos_test.h"
#include "dconfig.h"
#include "ducky.h"
#include "lint.h"
#include "unicode.h"
#include "layout.h"
#include <string.h>
#include <stdlib.h>

/* deterministic PRNG so a failure is reproducible */
static uint32_t rs = 0xC0FFEE;
static uint32_t rnd(void) { rs ^= rs << 13; rs ^= rs >> 17; rs ^= rs << 5; return rs; }

static void fill_random_text(char *buf, size_t cap)
{
    size_t n = rnd() % (cap - 1);
    for (size_t i = 0; i < n; i++) {
        uint32_t r = rnd() % 100;
        if (r < 60)      buf[i] = (char)(0x20 + (rnd() % 0x5F));  /* printable   */
        else if (r < 75) buf[i] = (char)(0x80 + (rnd() % 0x80));  /* high bytes  */
        else if (r < 85) buf[i] = '\n';
        else if (r < 92) buf[i] = ' ';
        else             buf[i] = (char)(rnd() % 0x20);           /* control     */
    }
    buf[n] = 0;
}

TEST_MAIN_BEGIN
    static char buf[4096];
    ducky_action_t acts[192];
    ducky_lint_t problems[4];

    SUITE("fuzz: config parser survives arbitrary bytes");
    {
        for (int i = 0; i < 3000; i++) {
            dolos_config_t c; config_defaults(&c);
            fill_random_text(buf, sizeof(buf));
            config_parse(buf, &c);
            /* whatever it parsed, the result must stay in range */
            CHECK_QUIET(c.layout < LAYOUT__COUNT, "layout stayed in range");
            CHECK_QUIET(c.os <= OS_MAC, "os stayed in range");
            /* the invariant that matters: every string is terminated INSIDE
             * its buffer, so strlen and friends can never run off the end */
            CHECK_QUIET(strnlen(c.arm_pin, sizeof(c.arm_pin)) < sizeof(c.arm_pin),
                        "arm_pin terminated in bounds");
            CHECK_QUIET(strnlen(c.wifi_ssid, sizeof(c.wifi_ssid)) < sizeof(c.wifi_ssid),
                        "ssid terminated in bounds");
            CHECK_QUIET(strnlen(c.wifi_pass, sizeof(c.wifi_pass)) < sizeof(c.wifi_pass),
                        "pass terminated in bounds");
            CHECK_QUIET(strnlen(c.admin_user, sizeof(c.admin_user)) < sizeof(c.admin_user),
                        "admin_user terminated in bounds");
            CHECK_QUIET(strnlen(c.usb_product, sizeof(c.usb_product)) < sizeof(c.usb_product),
                        "usb_product terminated in bounds");
        }
        CHECK(1, "3000 random configs parsed without a crash");
    }

    SUITE("fuzz: DuckyScript parser survives arbitrary lines");
    {
        for (int i = 0; i < 4000; i++) {
            ducky_state_t st; ducky_state_init(&st);
            st.layout = (kb_layout_t)(rnd() % LAYOUT__COUNT);
            st.target_os = (target_os_t)(rnd() % 3);
            char line[256];
            fill_random_text(line, sizeof(line));
            for (char *p = line; *p; p++) if (*p == '\n') *p = ' ';   /* single line */
            int n = ducky_parse_line(&st, line, acts, 192);
            CHECK_QUIET(n >= 0 && n <= 192, "action count stayed within the buffer");
        }
        CHECK(1, "4000 random lines parsed without overrunning the action buffer");
    }

    SUITE("fuzz: linter survives arbitrary payloads");
    {
        for (int i = 0; i < 1500; i++) {
            fill_random_text(buf, sizeof(buf));
            int n = ducky_lint(buf, (kb_layout_t)(rnd() % LAYOUT__COUNT),
                               (target_os_t)(rnd() % 3), problems, 4);
            CHECK_QUIET(n >= 0, "problem count is never negative");
        }
        CHECK(1, "1500 random payloads linted without a crash");
    }

    SUITE("fuzz: UTF-8 decoder never walks past the terminator");
    {
        for (int i = 0; i < 4000; i++) {
            char s[64];
            fill_random_text(s, sizeof(s));
            const char *p = s; uint32_t cp; int guard = 0;
            while (utf8_next(&p, &cp)) {
                CHECK_QUIET(p <= s + sizeof(s), "decoder stayed inside the buffer");
                if (++guard > 64) break;
            }
            CHECK_QUIET(p <= s + sizeof(s), "decoder ended inside the buffer");
        }
        CHECK(1, "4000 random strings decoded safely");
    }

    SUITE("fuzz: truncated multi-byte sequences terminate");
    {
        /* every prefix of a 4-byte codepoint, including the lone lead byte */
        const char *emoji = "\xF0\x9F\x8E\xAF";
        for (int cut = 1; cut <= 4; cut++) {
            char s[8]; memcpy(s, emoji, cut); s[cut] = 0;
            const char *p = s; uint32_t cp; int steps = 0;
            while (utf8_next(&p, &cp) && steps < 16) steps++;
            CHECK(steps < 16, "truncated %d-byte prefix terminated", cut);
        }
    }

    SUITE("fuzz: unicode_seq respects a tiny output buffer");
    {
        ducky_action_t small[3];
        for (uint32_t cp = 0x80; cp < 0x20000; cp += 977) {
            for (int os = 0; os < 3; os++) {
                int n = unicode_seq(cp, (target_os_t)os, small, 3);
                CHECK_QUIET(n <= 3, "never wrote past a 3-action buffer");
            }
        }
        CHECK(1, "sequences refused to overflow a short buffer");
    }

    SUITE("fuzz: degenerate inputs");
    {
        dolos_config_t c; config_defaults(&c);
        config_parse("", &c);                         CHECK(1, "empty config ok");
        config_parse("\n\n\n", &c);                   CHECK(1, "blank lines ok");
        config_parse("=", &c);                        CHECK(1, "lone '=' ok");
        config_parse("layout=", &c);                  CHECK(1, "empty value ok");
        config_parse("nokeyhere", &c);                CHECK(1, "no '=' ok");
        ducky_state_t st; ducky_state_init(&st);
        CHECK(ducky_parse_line(&st, "", acts, 192) == 0, "empty line emits nothing");
        CHECK(ducky_parse_line(&st, "STRING", acts, 192) == 0, "STRING with no text emits nothing");
        CHECK(ducky_parse_line(&st, "   ", acts, 192) == 0, "whitespace-only emits nothing");
        CHECK(ducky_lint("", LAYOUT_US, OS_WINDOWS, problems, 4) == 0, "empty payload is clean");
        CHECK(ducky_parse_line(&st, "STRING x", acts, 0) == 0, "zero-capacity buffer writes nothing");
    }
TEST_MAIN_END
