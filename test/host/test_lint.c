#include "dolos_test.h"
#include "lint.h"
#include <string.h>

static int lint(const char *txt, ducky_lint_t *e, kb_layout_t lay, target_os_t os)
{
    return ducky_lint(txt, lay, os, e, 4);
}

TEST_MAIN_BEGIN
    ducky_lint_t e[4];

    SUITE("lint: a good payload is clean");
    {
        const char *ok =
            "REM this is a comment\n"
            "# so is this\n"
            "DEFAULTDELAY 40\n"
            "DELAY 500\n"
            "GUI r\n"
            "STRING notepad\n"
            "ENTER\n"
            "STRINGLN hello\n"
            "CTRL ALT DELETE\n"
            "F5\n"
            "REPEAT 3\n";
        CHECK(lint(ok, e, LAYOUT_US, OS_WINDOWS) == 0, "clean payload reports no problems");
    }

    SUITE("lint: unknown command is caught with its line number");
    {
        const char *bad = "DELAY 100\nSTRING hi\nFLY TO THE MOON\nENTER\n";
        int n = lint(bad, e, LAYOUT_US, OS_WINDOWS);
        CHECK(n == 1, "one problem, got %d", n);
        CHECK(e[0].line == 3, "problem is on line 3, got %d", e[0].line);
        CHECK(strstr(e[0].msg, "unknown") != NULL, "says unknown command: %s", e[0].msg);
    }

    SUITE("lint: bad arguments");
    {
        CHECK(lint("DELAY abc\n", e, LAYOUT_US, OS_WINDOWS) == 1, "non-numeric DELAY");
        CHECK(lint("UNICODE zzzz\n", e, LAYOUT_US, OS_WINDOWS) == 1, "non-hex UNICODE");
        CHECK(lint("UNICODE 1F600\n", e, LAYOUT_US, OS_WINDOWS) == 0, "valid emoji codepoint on Windows");
        CHECK(lint("UNICODE 1F600\n", e, LAYOUT_US, OS_MAC) == 1, "same codepoint is out of range on macOS");
    }

    SUITE("lint: REPEAT needs something to repeat");
    {
        int n = lint("REPEAT 3\nSTRING hi\n", e, LAYOUT_US, OS_WINDOWS);
        CHECK(n == 1, "leading REPEAT is a problem, got %d", n);
        CHECK(e[0].line == 1, "flagged on line 1");
        CHECK(lint("STRING hi\nREPEAT 3\n", e, LAYOUT_US, OS_WINDOWS) == 0, "REPEAT after a command is fine");
        CHECK(lint("REPEAT x\n", e, LAYOUT_US, OS_WINDOWS) == 1, "REPEAT needs a count");
    }

    SUITE("lint: characters that cannot be typed");
    {
        /* a lone unpaired high byte is not valid UTF-8 and cannot be typed */
        CHECK(lint("STRING caf\xC3\xA9\n", e, LAYOUT_US, OS_WINDOWS) == 0,
              "accented text is fine - Unicode path handles it");
        CHECK(lint("STRING \xF0\x9F\x8E\xAF\n", e, LAYOUT_US, OS_MAC) == 1,
              "an emoji is not typable through the macOS BMP-only method");
        CHECK(lint("STRING \xF0\x9F\x8E\xAF\n", e, LAYOUT_US, OS_LINUX) == 0,
              "the same emoji is fine on Linux");
    }

    SUITE("lint: counts every problem but keeps only what fits");
    {
        const char *many = "NOPE\nDELAY x\nALSONOPE\nSTILLNO\nMOREBAD\nEVENMORE\n";
        ducky_lint_t two[2];
        int n = ducky_lint(many, LAYOUT_US, OS_WINDOWS, two, 2);
        CHECK(n == 6, "reports the true total (6), got %d", n);
        CHECK(two[0].line == 1, "first kept problem is the first one");
    }

    SUITE("lint: CRLF payloads (written on Windows) are accepted");
    {
        /* This is a real payload off an SD card. Splitting on '\n' alone leaves
         * a trailing '\r', which made "DELAY 3000" fail the numeric check and
         * reported "DELAY needs a number of ms" on line 1 of a perfectly good
         * file. Every line here ends CRLF. */
        const char *crlf =
            "DELAY 3000\r\n"
            "GUI r\r\n"
            "DELAY 800\r\n"
            "STRING CMD\r\n"
            "ENTER\r\n"
            "REPEAT 2\r\n";
        int n = ducky_lint(crlf, LAYOUT_US, OS_WINDOWS, e, 4);
        CHECK(n == 0, "CRLF payload should be clean, got %d problem(s): %s",
              n, n ? e[0].msg : "-");

        /* and a genuinely bad CRLF line is still caught, on the right line */
        const char *bad = "DELAY 3000\r\nDELAY abc\r\n";
        n = ducky_lint(bad, LAYOUT_US, OS_WINDOWS, e, 4);
        CHECK(n == 1 && e[0].line == 2, "bad CRLF line still flagged on line 2 (got %d, line %d)",
              n, n ? e[0].line : -1);

        /* lone-CR (classic Mac) endings must not merge the whole file into one line */
        CHECK(ducky_lint("DELAY 100\r\nSTRING hi\r\n", LAYOUT_US, OS_WINDOWS, e, 4) == 0,
              "mixed CRLF file is clean");
    }
TEST_MAIN_END
