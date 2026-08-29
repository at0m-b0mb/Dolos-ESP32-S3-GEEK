#include "lint.h"
#include "unicode.h"
#include "hid_keys.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

#define LINT_LINE_MAX 224     /* must match the player's line buffer */

static int lkw(const char *tok, const char *word)
{
    for (; *tok && *word; tok++, word++)
        if (toupper((unsigned char)*tok) != toupper((unsigned char)*word)) return 0;
    return *tok == 0 && *word == 0;
}

/* copy the first whitespace-delimited token of s into buf */
static const char *first_tok(const char *s, char *buf, size_t cap)
{
    size_t n = 0;
    while (*s == ' ' || *s == '\t') s++;
    while (*s && *s != ' ' && *s != '\t' && n < cap - 1) buf[n++] = *s++;
    buf[n] = 0;
    while (*s == ' ' || *s == '\t') s++;
    return s;                       /* remainder, leading space skipped */
}

static bool all_digits(const char *s)
{
    if (!*s) return false;
    for (; *s; s++) if (!isdigit((unsigned char)*s)) return false;
    return true;
}
static bool all_hex(const char *s)
{
    if (!*s) return false;
    for (; *s; s++) if (!isxdigit((unsigned char)*s)) return false;
    return true;
}

static void add(ducky_lint_t *out, int max, int *kept, int line, const char *msg)
{
    if (out && *kept < max) {
        out[*kept].line = line;
        snprintf(out[*kept].msg, sizeof(out[*kept].msg), "%s", msg);
        (*kept)++;
    }
}

int ducky_lint(const char *text, kb_layout_t layout, target_os_t os,
               ducky_lint_t *out, int max)
{
    ducky_state_t st; ducky_state_init(&st);
    st.layout = layout; st.target_os = os;

    ducky_action_t acts[192];
    char line[LINT_LINE_MAX], tok[32];
    int problems = 0, kept = 0, lineno = 0;
    bool any_cmd = false;                    /* has a repeatable command appeared? */

    const char *p = text ? text : "";
    while (*p) {
        size_t l = 0;
        bool truncated = false;
        while (*p && *p != '\n') {
            if (l < sizeof(line) - 1) line[l++] = *p;
            else truncated = true;
            p++;
        }
        line[l] = 0;
        if (*p == '\n') p++;
        lineno++;

        const char *rest = first_tok(line, tok, sizeof(tok));
        if (tok[0] == 0) continue;                       /* blank */
        if (tok[0] == '#' || lkw(tok, "REM")) continue;  /* comment */

        if (truncated) {
            problems++;
            add(out, max, &kept, lineno, "line too long, will be truncated");
        }

        /* --- targeted argument checks the runtime parser is lenient about --- */
        if (lkw(tok, "DELAY") || lkw(tok, "DEFAULTDELAY") || lkw(tok, "DEFAULT_DELAY")) {
            if (!all_digits(rest)) {
                problems++; add(out, max, &kept, lineno, "DELAY needs a number of ms");
            } else {
                any_cmd = true;
            }
            /* Handled here either way: DEFAULTDELAY only sets state and emits no
             * actions, so letting it reach the "produced nothing" check below
             * would report a false "unknown command". */
            continue;
        } else if (lkw(tok, "REPEAT")) {
            if (!all_digits(rest)) {
                problems++; add(out, max, &kept, lineno, "REPEAT needs a count");
                continue;
            }
            if (!any_cmd) {
                problems++; add(out, max, &kept, lineno, "REPEAT with no previous command");
                continue;
            }
        } else if (lkw(tok, "UNICODE")) {
            const char *h = rest;
            if ((h[0] == 'U' || h[0] == 'u') && h[1] == '+') h += 2;
            if (!all_hex(h)) {
                problems++; add(out, max, &kept, lineno, "UNICODE needs a hex codepoint");
                continue;
            }
            if (os == OS_MAC && strtoul(h, NULL, 16) > 0xFFFF) {
                problems++; add(out, max, &kept, lineno, "codepoint too high for macOS input");
                continue;
            }
        } else if (lkw(tok, "STRING") || lkw(tok, "STRINGLN")) {
            /* every character must be typable: on the layout, or via Unicode */
            const char *q = rest; uint32_t cp; bool bad = false;
            while (utf8_next(&q, &cp) && !bad) {
                if (cp < 0x80) {
                    uint8_t k, m;
                    if (!hid_from_ascii_layout((char)cp, layout, &k, &m)) bad = true;
                } else {
                    ducky_action_t probe[24];
                    if (unicode_seq(cp, os, probe, 24) == 0) bad = true;
                }
            }
            if (bad) {
                problems++; add(out, max, &kept, lineno, "text has a character it cannot type");
                continue;
            }
            any_cmd = true;
            continue;
        }

        /* --- everything else: the real parser decides whether it means anything --- */
        int n = ducky_parse_line(&st, line, acts, (int)(sizeof(acts) / sizeof(acts[0])));
        if (n == 0 && st.repeat == 0) {
            problems++; add(out, max, &kept, lineno, "unknown command");
            continue;
        }
        any_cmd = true;
    }
    return problems;
}
