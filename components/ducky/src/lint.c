#include "lint.h"
#include "unicode.h"
#include "dscript.h"
#include "hid_keys.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#define LINT_LINE_MAX 8192     /* must match the player's line buffer */

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

/* Does this look like a variable, constant or expression rather than a literal?
 * Anything starting with $ or #, or a bare identifier, or something with an
 * operator in it. Deliberately generous: the language layer evaluates it, and
 * the linter's job here is to avoid crying wolf over a perfectly good payload. */
/* Copy just the first whitespace-delimited word of s. "REPEAT 4 TAB" carries
 * its count in the first token and a command in the rest, so the count must be
 * checked on its own rather than on the whole remainder. */
static void first_word(const char *s, char *out, size_t cap)
{
    size_t n = 0;
    while (*s == ' ' || *s == '\t') s++;
    while (*s && *s != ' ' && *s != '\t' && n < cap - 1) out[n++] = *s++;
    out[n] = 0;
}

static bool is_value_ref(const char *s)
{
    if (!s || !*s) return false;
    if (*s == '$' || *s == '#' || *s == '(') return true;   /* variable, constant, expression */
    for (const char *p = s; *p; p++)
        if (*p == '$' || *p == '#' || *p == '(') return true;
    /* A BARE name is accepted only if it is written as a constant - upper case
     * and underscores, as every DEFINE in the official library is. That keeps
     * "DELAY STARTUP_DELAY" working while still catching "DELAY x", which is
     * far more likely to be a typo than a reference. */
    bool has_alpha = false;
    for (const char *p = s; *p; p++) {
        if (islower((unsigned char)*p)) return false;
        if (isalpha((unsigned char)*p)) has_alpha = true;
        else if (*p != '_' && !isdigit((unsigned char)*p)) return false;
    }
    return has_alpha;
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
    static DOLOS_BIG_BSS ducky_state_t st; ducky_state_init(&st);
    st.layout = layout; st.target_os = os;

    /* The linter only asks whether a line parses to anything, and the commands
     * that reach this call emit a handful of actions at most (STRING and the
     * delays are handled above). 32 is ample and keeps 2.5 KB off the caller's
     * stack - lint runs on the UI task and on the console's HTTP task. */
    ducky_action_t acts[32];
    /* Static, and deliberately so: 8 KB of line buffer plus a ducky_state_t
     * cannot sit on the stack of the UI task. Every caller of ducky_lint()
     * holds the app lock, so there is one linter at a time. */
    static DOLOS_BIG_BSS char line[LINT_LINE_MAX];
    char tok[40];
    int problems = 0, kept = 0, lineno = 0;
    int in_block = 0;      /* inside STRING/STRINGLN ... END_STRING(LN) */
    bool any_cmd = false;                    /* has a repeatable command appeared? */
    bool in_rem = false;                     /* inside REM_BLOCK ... END_REM       */

    /* Structural problems (an IF with no END_IF) are found by the interpreter
     * itself, so the linter and the runtime cannot disagree about them. */
    static dscript_t probe;
    if (!dscript_init(&probe, text ? text : "")) {
        add(out, max, &kept, dscript_error_line(&probe), dscript_error(&probe));
        return 1;
    }

    const char *p = text ? text : "";
    /* A UTF-8 byte-order mark at the start of the file would otherwise glue
     * itself to the first keyword and make line 1 an unknown command. */
    if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;

    /* Pre-scan the FUNCTION definitions. A payload that defines
     * Rolling_Powershell_Execution() and then calls it is calling something
     * perfectly real, and the linter has to know that before it judges the
     * call site. */
    char fname[24][32]; int nfn = 0;
    for (const char *q = p; *q && nfn < 24; ) {
        const char *e2 = q; while (*e2 && *e2 != '\n') e2++;
        const char *t = q; while (*t == ' ' || *t == '\t') t++;
        if (strncmp(t, "FUNCTION", 8) == 0) {
            t += 8; while (*t == ' ' || *t == '\t') t++;
            int k = 0;
            while (t < e2 && (isalnum((unsigned char)*t) || *t == '_') && k < 31) fname[nfn][k++] = *t++;
            fname[nfn][k] = 0;
            if (k) nfn++;
        }
        q = (*e2 == '\n') ? e2 + 1 : e2;
    }
    while (*p) {
        size_t l = 0;
        bool truncated = false;
        while (*p && *p != '\n') {
            if (l < sizeof(line) - 1) line[l++] = *p;
            else truncated = true;
            p++;
        }
        while (l > 0 && line[l - 1] == '\r') l--;   /* CRLF payloads */
        line[l] = 0;
        if (*p == '\n') p++;
        lineno++;

        /* Everything between a block opener and its END_ is TEXT TO TYPE, not
         * script: PowerShell, bash, whatever the payload is writing out. The
         * linter used to read those lines as commands and report half a shell
         * script as unknown commands. */
        {
            const char *b = line;
            while (*b == ' ' || *b == '\t') b++;
            if (in_block) {
                if (strncasecmp(b, "END_STRINGLN", 12) == 0 || strncasecmp(b, "END_STRING", 10) == 0)
                    in_block = 0;
                continue;
            }
            static const char *OPEN[] = { "STRINGLN_POWERSHELL","STRINGLN_BLOCK","STRINGLN_BASH",
                                          "STRING_POWERSHELL","STRING_BLOCK","STRING_BASH", NULL };
            bool opened = false;
            for (int q = 0; OPEN[q]; q++) {
                size_t ln2 = strlen(OPEN[q]);
                if (strncasecmp(b, OPEN[q], ln2) == 0) {
                    const char *after = b + ln2;
                    while (*after == ' ' || *after == '\t' || *after == '\r') after++;
                    if (*after == 0) { opened = true; break; }
                }
            }
            /* the bare keyword alone on its line also opens a block */
            if (!opened && (strncasecmp(b, "STRINGLN", 8) == 0 || strncasecmp(b, "STRING", 6) == 0)) {
                const char *after = b + (strncasecmp(b, "STRINGLN", 8) == 0 ? 8 : 6);
                while (*after == ' ' || *after == '\t' || *after == '\r') after++;
                if (*after == 0) opened = true;      /* "STRING" with no text */
            }
            if (opened) { in_block = 1; any_cmd = true; continue; }
        }

        /* Control-flow and library constructs are handled by the interpreter,
         * not the line parser. The linter used to know only the keywords it
         * had been told about individually, so every construct added to the
         * language reappeared here as "unknown command". Ask the interpreter
         * instead - one source of truth. */
        {
            const char *t2 = line;
            while (*t2 == ' ' || *t2 == '\t') t2++;
            static const char *DS_CONSUMED[] = {
                "EXTENSION","END_EXTENSION","STAGE","END_STAGE",
                "IF_DEFINED_TRUE","IF_DEFINED_FALSE","END_IF_DEFINED",
                "BUTTON_DEF","END_BUTTON","INJECT_VAR",
                "RESTART_PAYLOAD","STOP_PAYLOAD",
                "STRING_BLOCK","STRINGLN_BLOCK","STRING_BASH","STRINGLN_BASH",
                "STRING_POWERSHELL","STRINGLN_POWERSHELL",
                "END_STRING","END_STRINGLN","ELSE_DEFINED","IF_NOT_DEFINED_TRUE",
                "IF_NOT_DEFINED_FALSE","EXIT", NULL };
            bool consumed = false;
            for (int q = 0; DS_CONSUMED[q]; q++) {
                size_t ln = strlen(DS_CONSUMED[q]);
                if (strncasecmp(t2, DS_CONSUMED[q], ln) == 0 &&
                    (t2[ln] == 0 || t2[ln] == ' ' || t2[ln] == '\t' || t2[ln] == '\r')) {
                    consumed = true; break;
                }
            }
            if (consumed) { any_cmd = true; continue; }
        }

        const char *rest = first_tok(line, tok, sizeof(tok));
        if (tok[0] == 0) continue;                       /* blank */
        if (tok[0] == '#' || lkw(tok, "REM")) continue;  /* comment */
        /* "REM:" and "REM<" - a remark with punctuation is still a remark. */
        if ((tok[0] == 'R' || tok[0] == 'r') && (tok[1] == 'E' || tok[1] == 'e') &&
            (tok[2] == 'M' || tok[2] == 'm') && tok[3] && !isalnum((unsigned char)tok[3]) &&
            tok[3] != '_') continue;
        /* a call to a function this payload defines */
        {
            char base[32]; size_t bl = 0;
            for (const char *q2 = tok; *q2 && *q2 != '(' && bl < sizeof(base) - 1; q2++) base[bl++] = *q2;
            base[bl] = 0;
            bool is_call = false;
            for (int q3 = 0; q3 < nfn; q3++) if (strcmp(base, fname[q3]) == 0) { is_call = true; break; }
            if (is_call) { any_cmd = true; continue; }
        }
        if (dscript_is_consumed(&probe, line)) { any_cmd = true; continue; }
        if (lkw(tok, "REM_BLOCK")) { in_rem = true; continue; }
        if (lkw(tok, "END_REM"))   { in_rem = false; continue; }
        if (in_rem) continue;

        if (truncated) {
            problems++;
            add(out, max, &kept, lineno, "line too long, will be truncated");
        }

        /* --- targeted argument checks the runtime parser is lenient about --- */
        if (lkw(tok, "DELAY") || lkw(tok, "DEFAULTDELAY") || lkw(tok, "DEFAULT_DELAY") ||
            lkw(tok, "STRINGDELAY") || lkw(tok, "STRING_DELAY")) {
            /* "DELAY 500", but equally "DELAY #RESPONSE_DELAY" or
             * "DELAY STARTUP_DELAY": the value is a constant the language layer
             * resolves at run time, and the linter cannot evaluate it. */
            char w[40]; first_word(rest, w, sizeof(w));
            if (w[0] == 0) { any_cmd = true; continue; }   /* bare DELAY = default */
            if (!all_digits(w) && is_value_ref(w)) { any_cmd = true; continue; }
            if (!all_digits(w)) {
                problems++; add(out, max, &kept, lineno, "DELAY needs a number of ms");
            } else {
                any_cmd = true;
            }
            /* Handled here either way: DEFAULTDELAY only sets state and emits no
             * actions, so letting it reach the "produced nothing" check below
             * would report a false "unknown command". */
            continue;
        } else if (lkw(tok, "REPEAT")) {
            /* "REPEAT 3" and "REPEAT 4 TAB" are both valid; only the count is
             * checked here, and whatever follows is an ordinary command. */
            char w[40]; first_word(rest, w, sizeof(w));
            if (!all_digits(w) && is_value_ref(w)) { any_cmd = true; continue; }
            if (!all_digits(w)) {
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
                    uint8_t uk, um;
                    uint8_t dk, dm, bk, bm;
                    if (layout_utf8_key(layout, cp, &uk, &um)) continue;  /* on the layout */
                    if (layout_utf8_combo(layout, cp, &dk, &dm, &bk, &bm)) continue;
                    ducky_action_t probe2[24];
                    if (unicode_seq(cp, os, probe2, 24) == 0) bad = true;
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
