#include "dolos_test.h"
#include "dscript.h"
#include <string.h>
#include <stdio.h>

/* Collect every line the interpreter decides should actually type, joined by
 * '|'. That is the whole observable behaviour of the language layer, so most
 * assertions are just "running this program yields these lines". */
static const char *run(const char *src, char *out, size_t cap, dscript_t *ds)
{
    dscript_init(ds, src);
    out[0] = 0;
    const char *l;
    size_t o = 0;
    while ((l = dscript_next(ds)) != NULL) {
        int n = snprintf(out + o, cap - o, "%s%s", o ? "|" : "", l);
        if (n < 0 || (size_t)n >= cap - o) break;
        o += (size_t)n;
    }
    return out;
}

TEST_MAIN_BEGIN
    char out[1024];
    dscript_t ds;

    SUITE("dscript: plain payloads pass straight through");
    {
        run("STRING hello\nENTER\nSTRING bye\n", out, sizeof(out), &ds);
        CHECK(strcmp(out, "STRING hello|ENTER|STRING bye") == 0, "got '%s'", out);
        CHECK(dscript_error(&ds) == NULL, "no error");
    }

    SUITE("dscript: VAR, arithmetic and substitution into STRING");
    {
        run("VAR $a = 2\nVAR $b = $a * 3 + 1\nSTRING value=$b\n", out, sizeof(out), &ds);
        CHECK(strcmp(out, "STRING value=7") == 0, "2*3+1 substituted, got '%s'", out);
        int32_t v = 0;
        CHECK(dscript_get(&ds, "b", &v) && v == 7, "b == 7, got %d", (int)v);

        /* precedence, parentheses, and the rest of the operator table */
        run("VAR $x = (2 + 3) * 4\nVAR $y = 10 % 4\nVAR $z = 1 << 5\n"
            "VAR $w = 7 & 3\nVAR $q = 2 ^ 8\nSTRING $x,$y,$z,$w,$q\n", out, sizeof(out), &ds);
        CHECK(strcmp(out, "STRING 20,2,32,3,256") == 0, "operator table, got '%s'", out);
    }

    SUITE("dscript: assignment to an existing variable");
    {
        run("VAR $i = 0\n$i = $i + 5\n$i = $i * 2\nSTRING $i\n", out, sizeof(out), &ds);
        CHECK(strcmp(out, "STRING 10") == 0, "reassignment works, got '%s'", out);
    }

    SUITE("dscript: IF / ELSE / END_IF");
    {
        run("VAR $n = 5\nIF ($n > 3) THEN\nSTRING big\nELSE\nSTRING small\nEND_IF\nSTRING done\n",
            out, sizeof(out), &ds);
        CHECK(strcmp(out, "STRING big|STRING done") == 0, "true branch only, got '%s'", out);

        run("VAR $n = 1\nIF ($n > 3) THEN\nSTRING big\nELSE\nSTRING small\nEND_IF\nSTRING done\n",
            out, sizeof(out), &ds);
        CHECK(strcmp(out, "STRING small|STRING done") == 0, "false branch only, got '%s'", out);

        /* no ELSE at all */
        run("VAR $n = 0\nIF ($n) THEN\nSTRING never\nEND_IF\nSTRING after\n", out, sizeof(out), &ds);
        CHECK(strcmp(out, "STRING after") == 0, "skips a bodyless IF, got '%s'", out);

        /* nesting: the inner END_IF must not close the outer one */
        run("VAR $a = 1\nVAR $b = 1\n"
            "IF ($a) THEN\nIF ($b) THEN\nSTRING inner\nEND_IF\nSTRING outer\nEND_IF\nSTRING end\n",
            out, sizeof(out), &ds);
        CHECK(strcmp(out, "STRING inner|STRING outer|STRING end") == 0,
              "nested IF, got '%s'", out);

        /* a false OUTER if must skip the whole nested block */
        run("VAR $a = 0\nIF ($a) THEN\nIF (1) THEN\nSTRING no\nEND_IF\nSTRING also-no\nEND_IF\nSTRING yes\n",
            out, sizeof(out), &ds);
        CHECK(strcmp(out, "STRING yes") == 0, "false outer skips everything, got '%s'", out);
    }

    SUITE("dscript: WHILE loops, including one that never runs");
    {
        run("VAR $i = 0\nWHILE ($i < 3)\nSTRING n=$i\n$i = $i + 1\nEND_WHILE\nSTRING done\n",
            out, sizeof(out), &ds);
        CHECK(strcmp(out, "STRING n=0|STRING n=1|STRING n=2|STRING done") == 0,
              "three iterations, got '%s'", out);

        run("VAR $i = 9\nWHILE ($i < 3)\nSTRING never\nEND_WHILE\nSTRING after\n",
            out, sizeof(out), &ds);
        CHECK(strcmp(out, "STRING after") == 0, "a false loop runs zero times, got '%s'", out);

        /* nested loops */
        run("VAR $i = 0\nWHILE ($i < 2)\nVAR $j = 0\nWHILE ($j < 2)\nSTRING $i$j\n"
            "$j = $j + 1\nEND_WHILE\n$i = $i + 1\nEND_WHILE\n", out, sizeof(out), &ds);
        CHECK(strcmp(out, "STRING 00|STRING 01|STRING 10|STRING 11") == 0,
              "nested loops, got '%s'", out);
    }

    SUITE("dscript: FUNCTION definitions do not run inline, calls do");
    {
        run("FUNCTION greet()\nSTRING hi\nEND_FUNCTION\nSTRING start\ngreet\nSTRING end\n",
            out, sizeof(out), &ds);
        CHECK(strcmp(out, "STRING start|STRING hi|STRING end") == 0,
              "definition skipped, call executed, got '%s'", out);

        /* called twice, and callable before its definition appears */
        run("STRING a\ntwice\ntwice\nFUNCTION twice()\nSTRING x\nEND_FUNCTION\nSTRING b\n",
            out, sizeof(out), &ds);
        CHECK(strcmp(out, "STRING a|STRING x|STRING x|STRING b") == 0,
              "forward call, got '%s'", out);

        /* a function that mutates state the caller can see */
        run("VAR $c = 0\nFUNCTION bump()\n$c = $c + 10\nEND_FUNCTION\nbump\nbump\nSTRING $c\n",
            out, sizeof(out), &ds);
        CHECK(strcmp(out, "STRING 20") == 0, "shared variables, got '%s'", out);
    }

    SUITE("dscript: bounded - a runaway loop stops instead of hanging");
    {
        run("VAR $i = 0\nWHILE (1)\n$i = $i + 1\nEND_WHILE\n", out, sizeof(out), &ds);
        CHECK(dscript_error(&ds) != NULL, "an endless loop is reported, not run for ever");
    }

    SUITE("dscript: structural mistakes are reported, not crashed on");
    {
        run("IF (1) THEN\nSTRING x\n", out, sizeof(out), &ds);
        CHECK(dscript_error(&ds) != NULL, "IF without END_IF is an error");

        run("WHILE (1)\nSTRING x\n", out, sizeof(out), &ds);
        CHECK(dscript_error(&ds) != NULL, "WHILE without END_WHILE is an error");

        run("VAR $a = $nope + 1\n", out, sizeof(out), &ds);
        CHECK(dscript_error(&ds) != NULL, "an unknown variable is an error");

        run("VAR $a = 1 / 0\n", out, sizeof(out), &ds);
        CHECK(dscript_error(&ds) != NULL, "division by zero is an error, not a crash");
    }

    SUITE("dscript: control keywords are recognised as such");
    {
        CHECK(dscript_is_control("IF ($x)"), "IF");
        CHECK(dscript_is_control("  END_WHILE"), "indented END_WHILE");
        CHECK(dscript_is_control("VAR $a = 1"), "VAR");
        CHECK(!dscript_is_control("STRING IF"), "a STRING is not control flow");
        CHECK(!dscript_is_control("ENTER"), "ENTER is not control flow");
    }

    SUITE("dscript: a real-world shaped payload");
    {
        run("REM count down and finish\n"
            "VAR $n = 3\n"
            "WHILE ($n > 0)\n"
            "STRING tick $n\n"
            "ENTER\n"
            "$n = $n - 1\n"
            "END_WHILE\n"
            "IF ($n == 0) THEN\n"
            "STRING liftoff\n"
            "END_IF\n", out, sizeof(out), &ds);
        /* the REM is not yielded: it types nothing, so it is not a step */
        CHECK(strcmp(out,
              "STRING tick 3|ENTER|STRING tick 2|ENTER|"
              "STRING tick 1|ENTER|STRING liftoff") == 0, "got '%s'", out);
        CHECK(dscript_error(&ds) == NULL, "and no error");
    }

    SUITE("dscript: the executed-line count is what progress should use");
    {
        /* A file's line count and its EXECUTED line count are different things,
         * and using the former made the on-screen progress wrong in both
         * directions: stalled during loops, and past the end afterwards. */
        const char *src =
            "REM a comment\n"          /* not executed */
            "\n"                        /* blank        */
            "VAR $i = 0\n"             /* consumed     */
            "WHILE ($i < 3)\n"         /* consumed     */
            "STRING x\n"               /* EXECUTED x3  */
            "$i = $i + 1\n"            /* consumed     */
            "END_WHILE\n"              /* consumed     */
            "STRING done\n";           /* EXECUTED x1  */
        dscript_init(&ds, src);
        int n = 0;
        while (dscript_next(&ds) != NULL) n++;
        CHECK(n == 4, "8 file lines but 4 executed (3 loop + 1), got %d", n);
    }

    SUITE("dscript: DEFINE is a TEXT macro, not an arithmetic assignment");
    {
        /* This is the single biggest compatibility fact about DuckyScript, and
         * getting it wrong failed 136 of the 253 official payloads: almost no
         * DEFINE in the library holds a number. */
        static dscript_t d;
        const char *p1 =
            "DEFINE #SCRIPT_URL https://example.com/a.ps1\n"
            "DEFINE SUDO_PASS hunter2\n"
            "STRING curl #SCRIPT_URL\n"
            "STRING pw is SUDO_PASS\n";
        dscript_init(&d, p1);
        const char *l = dscript_next(&d);
        CHECK(l && strcmp(l, "STRING curl https://example.com/a.ps1") == 0,
              "#NAME is replaced by its text, got [%s]", l ? l : "(null)");
        l = dscript_next(&d);
        CHECK(l && strcmp(l, "STRING pw is hunter2") == 0,
              "a DEFINE without a sigil works too, got [%s]", l ? l : "(null)");
        CHECK(dscript_error(&d) == NULL, "no error: %s", dscript_error(&d) ? dscript_error(&d) : "");

        /* a numeric DEFINE still works in arithmetic, because the substituted
         * text simply parses as a number */
        static dscript_t d2;
        dscript_init(&d2, "DEFINE #N 3\nVAR $x = (#N * 2)\n");
        while (dscript_next(&d2)) {}
        int32_t v = 0;
        CHECK(dscript_get(&d2, "x", &v) && v == 6, "numeric DEFINE still computes, got %ld", (long)v);

        /* substitution respects token boundaries: #FOO must not match #FOOBAR */
        static dscript_t d3;
        dscript_init(&d3, "DEFINE #A one\nDEFINE #AB two\nSTRING #AB\n");
        l = dscript_next(&d3);
        CHECK(l && strcmp(l, "STRING two") == 0, "#AB is not mangled by #A, got [%s]", l ? l : "");
    }

    SUITE("dscript: long system-variable names are not truncated");
    {
        /* $_HOST_CONFIGURATION_REQUEST_COUNT is 33 characters. A 16-byte name
         * buffer split it in two and reported the tail as an unknown variable. */
        static dscript_t d;
        dscript_init(&d, "VAR $n = 0\nIF ($_HOST_CONFIGURATION_REQUEST_COUNT > 1) THEN\n $n = 5\nEND_IF\n");
        dscript_set_host_usb(&d, 3, 1);
        while (dscript_next(&d)) {}
        int32_t v = 0;
        CHECK(dscript_error(&d) == NULL, "no error: %s", dscript_error(&d) ? dscript_error(&d) : "");
        CHECK(dscript_get(&d, "n", &v) && v == 5, "the 33-character system variable resolved, n=%ld", (long)v);

        /* any other $_ name exists and reads as zero rather than erroring */
        static dscript_t d2;
        dscript_init(&d2, "VAR $k = ($_EXFIL_MODE_ENABLED + 1)\n");
        while (dscript_next(&d2)) {}
        CHECK(dscript_error(&d2) == NULL, "unset $_ variables are zero, not errors");
    }

    SUITE("dscript: a function can return a value into a variable");
    {
        /* "$X = FUNC()" is how the official payloads capture a result. The call
         * runs as an ordinary statement - so anything it types still gets typed
         * - and RETURN assigns into the waiting variable. */
        static dscript_t d;
        dscript_init(&d,
            "FUNCTION GET_VAL()\n"
            "    VAR $tmp = 7\n"
            "    RETURN $tmp\n"
            "END_FUNCTION\n"
            "VAR $HEX = 0\n"
            "$HEX = GET_VAL()\n");
        while (dscript_next(&d)) {}
        int32_t v = -1;
        CHECK(dscript_error(&d) == NULL, "no error: %s", dscript_error(&d) ? dscript_error(&d) : "");
        CHECK(dscript_get(&d, "HEX", &v) && v == 7, "the returned value was stored, got %ld", (long)v);

        /* a function that types still types when called this way */
        static dscript_t d2;
        dscript_init(&d2,
            "FUNCTION SAY()\n"
            "    STRING hi\n"
            "    RETURN 3\n"
            "END_FUNCTION\n"
            "VAR $n = 0\n"
            "$n = SAY()\n");
        const char *l = dscript_next(&d2);
        CHECK(l && strcmp(l, "STRING hi") == 0, "the body still runs, got [%s]", l ? l : "(null)");
        while (dscript_next(&d2)) {}
        int32_t n2 = -1;
        CHECK(dscript_get(&d2, "n", &n2) && n2 == 3, "and the value comes back, got %ld", (long)n2);

        /* an 8 KB line survives: the official library has base64 blobs at 6.5 KB */
        static char big[9000];
        int o = snprintf(big, sizeof(big), "STRING ");
        for (int i = 0; i < 6800; i++) big[o++] = 'a';
        big[o++] = '\n'; big[o] = 0;
        static dscript_t d3;
        dscript_init(&d3, big);
        l = dscript_next(&d3);
        CHECK(l && strlen(l) > 6000, "a 6.8 KB line is not truncated, got %u chars",
              (unsigned)(l ? strlen(l) : 0));
    }
TEST_MAIN_END
