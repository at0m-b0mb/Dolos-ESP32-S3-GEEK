#include "dscript.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

/* ---------------------------------------------------------------- helpers */

static int ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 0;
    return *a == 0 && *b == 0;
}

/* Does `line` begin with keyword `kw`, followed by end-of-line or a space? */
/* The keyword alone on a line (trailing spaces allowed) - which is how a
 * STRING block is opened, as opposed to "STRING some text". */
static bool is_bare_kw(const char *l, const char *kw)
{
    size_t n = strlen(kw);
    for (size_t i = 0; i < n; i++)
        if (toupper((unsigned char)l[i]) != kw[i]) return false;
    const char *r = l + n;
    while (*r == ' ' || *r == '\t' || *r == '\r') r++;
    return *r == 0;
}

static bool starts_with_kw(const char *line, const char *kw)
{
    size_t n = strlen(kw);
    /* Case SENSITIVE, deliberately. DuckyScript keywords are upper case, and
     * payloads routinely type lower-case shell and PowerShell that contains
     * "if", "while" and "return". Matching those as control flow produced
     * "IF without END_IF" against perfectly correct files. */
    for (size_t i = 0; i < n; i++)
        if (line[i] != kw[i]) return false;
    /* A parenthesis counts as a delimiter: the official payloads write both
     * "WHILE (cond)" and "WHILE(cond)", and treating the second as an unknown
     * word left its END_WHILE dangling - which is what "END_WHILE without
     * WHILE" really was, forty times over. */
    return line[n] == 0 || line[n] == ' ' || line[n] == '\t' ||
           line[n] == '(' || line[n] == '\r';
}

static const char *skip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Copy one indexed line into buf, trimmed of surrounding whitespace and CR. */
static void get_line(const dscript_t *ds, uint16_t i, char *buf, size_t cap)
{
    buf[0] = 0;
    if (i >= ds->nlines) return;
    size_t len = ds->line[i].len;
    if (len > cap - 1) len = cap - 1;
    memcpy(buf, ds->text + ds->line[i].off, len);
    buf[len] = 0;
    while (len > 0 && (buf[len - 1] == '\r' || buf[len - 1] == ' ' || buf[len - 1] == '\t'))
        buf[--len] = 0;
}

static void fail(dscript_t *ds, const char *why)
{
    if (!ds->err) { ds->err = why; ds->err_line = (uint16_t)(ds->pc); }
}

/* ------------------------------------------------------------- variables */

static int var_index(const dscript_t *ds, const char *name)
{
    for (int i = 0; i < ds->nvars; i++)
        if (ieq(ds->var[i].name, name)) return i;
    return -1;
}

static bool var_set(dscript_t *ds, const char *name, int32_t v)
{
    int i = var_index(ds, name);
    if (i < 0) {
        if (ds->nvars >= DS_MAX_VARS) { fail(ds, "too many variables"); return false; }
        i = ds->nvars++;
        snprintf(ds->var[i].name, sizeof(ds->var[i].name), "%s", name);
    }
    ds->var[i].val = v;
    return true;
}

void dscript_set_host_usb(dscript_t *ds, int32_t cfg_requests, uint8_t lock_reply)
{
    ds->host_cfg_requests = cfg_requests;
    ds->host_lock_reply = lock_reply;
}

void dscript_set_host(dscript_t *ds, int32_t os, uint8_t leds, uint8_t button_pushed)
{
    ds->host_os = os;
    ds->host_leds = leds;
    ds->button_pushed = button_pushed;
}

bool dscript_get(const dscript_t *ds, const char *name, int32_t *out)
{
    if (*name == '$' || *name == '#') name++;
    int i = var_index(ds, name);
    if (i < 0) return false;
    if (out) *out = ds->var[i].val;
    return true;
}

/* The $_ system variables: facts about the machine, not values the script set.
 * They are resolved here rather than stored, so they are always current - a
 * payload that waits for CAPS LOCK and then reads $_CAPSLOCK_ON must see what
 * the host is doing now, not what it was when the line was parsed. */
static bool sys_var(dscript_t *ds, const char *name, int32_t *out)
{
    if (name[0] != '_') return false;
    if (ieq(name, "_OS"))            { *out = ds->host_os; return true; }
    if (ieq(name, "_CAPSLOCK_ON"))   { *out = (ds->host_leds & 0x02) ? 1 : 0; return true; }
    if (ieq(name, "_NUMLOCK_ON"))    { *out = (ds->host_leds & 0x01) ? 1 : 0; return true; }
    if (ieq(name, "_SCROLLLOCK_ON")) { *out = (ds->host_leds & 0x04) ? 1 : 0; return true; }
    if (ieq(name, "_SAVED_CAPSLOCK_ON"))   { *out = (ds->saved_leds & 0x02) ? 1 : 0; return true; }
    if (ieq(name, "_SAVED_NUMLOCK_ON"))    { *out = (ds->saved_leds & 0x01) ? 1 : 0; return true; }
    if (ieq(name, "_SAVED_SCROLLLOCK_ON")) { *out = (ds->saved_leds & 0x04) ? 1 : 0; return true; }
    if (ieq(name, "_BUTTON_PUSH_RECEIVED")) { *out = ds->button_pushed ? 1 : 0; return true; }
    if (ieq(name, "_HOST_CONFIGURATION_REQUEST_COUNT")) { *out = ds->host_cfg_requests; return true; }
    if (ieq(name, "_RECEIVED_HOST_LOCK_LED_REPLY"))     { *out = ds->host_lock_reply ? 1 : 0; return true; }
    /* Random keycodes, as HID usage ids, for payloads that build their own
     * strings a character at a time. */
    if (ieq(name, "_RANDOM_LOWER_LETTER_KEYCODE") || ieq(name, "_RANDOM_UPPER_LETTER_KEYCODE") ||
        ieq(name, "_RANDOM_LETTER_KEYCODE")) {
        uint32_t r = ds->rnd ? ds->rnd() : (ds->rnd_ctr = ds->rnd_ctr * 1664525u + 1013904223u);
        *out = 0x04 + (int32_t)(r % 26);            /* HID a..z */
        return true;
    }
    if (ieq(name, "_RANDOM_NUMBER_KEYCODE")) {
        uint32_t r = ds->rnd ? ds->rnd() : (ds->rnd_ctr = ds->rnd_ctr * 1664525u + 1013904223u);
        *out = 0x1E + (int32_t)(r % 10);            /* HID 1..0 */
        return true;
    }
    /* A value the payload set explicitly wins over the default for the
     * writable ones; the read-only host facts above never reach here. */
    if (ieq(name, "_RANDOM_MIN") || ieq(name, "_RANDOM_MAX") ||
        ieq(name, "_EXFIL_MODE_ENABLED") || ieq(name, "_EXFIL_LEDS_ENABLED") ||
        ieq(name, "_JITTER_ENABLED") || ieq(name, "_JITTER_MAX")) {
        int i = var_index(ds, name);
        *out = (i >= 0) ? ds->var[i].val : (ieq(name, "_RANDOM_MAX") ? 65535 : 0);
        return true;
    }
    if (ieq(name, "_RANDOM_INT")) {
        int32_t lo = 0, hi = 65535;
        int i = var_index(ds, "_RANDOM_MIN"); if (i >= 0) lo = ds->var[i].val;
        i = var_index(ds, "_RANDOM_MAX");     if (i >= 0) hi = ds->var[i].val;
        if (hi < lo) { int32_t t = lo; lo = hi; hi = t; }
        uint32_t r = ds->rnd ? ds->rnd() : (ds->rnd_ctr = ds->rnd_ctr * 1664525u + 1013904223u);
        *out = lo + (int32_t)(r % (uint32_t)(hi - lo + 1));
        return true;
    }
    /* Every other $_ name still EXISTS - on the real device they are all
     * defined, and several ($_RANDOM_MAX, $_EXFIL_MODE_ENABLED, $_JITTER_MAX)
     * are written by payloads before they are read. So: a stored value wins,
     * and an unread one is zero rather than an error. Treating them as unknown
     * variables was rejecting 137 of the 253 official payloads outright. */
    {
        int i = var_index(ds, name);
        *out = (i >= 0) ? ds->var[i].val : 0;
        return true;
    }
}

/* ------------------------------------------------------------ expressions
 *
 * Recursive descent, lowest precedence first, matching the operator table in
 * the DuckyScript reference:
 *   ||  &&  | &  == != < > <= >=  << >>  + -  * / %  ^  unary - !  ( )
 * Values are int32 internally; DuckyScript's own range is 0..65535, which fits
 * comfortably and leaves room for intermediate results to go negative.
 */
typedef struct { dscript_t *ds; const char *p; } ex_t;

static int32_t ex_or(ex_t *e);

static void ex_ws(ex_t *e) { e->p = skip_ws(e->p); }

static bool ex_eat(ex_t *e, const char *op)
{
    ex_ws(e);
    size_t n = strlen(op);
    if (strncmp(e->p, op, n) == 0) {
        /* do not let "<" swallow the "<" of "<<" or "<=" */
        if (n == 1 && (op[0] == '<' || op[0] == '>') &&
            (e->p[1] == op[0] || e->p[1] == '=')) return false;
        if (n == 1 && (op[0] == '&' || op[0] == '|') && e->p[1] == op[0]) return false;
        if (n == 1 && op[0] == '=' && e->p[1] == '=') return false;
        e->p += n;
        return true;
    }
    return false;
}

static int32_t ex_atom(ex_t *e)
{
    ex_ws(e);
    if (*e->p == '(') {
        e->p++;
        int32_t v = ex_or(e);
        ex_ws(e);
        if (*e->p == ')') e->p++;
        else fail(e->ds, "missing )");
        return v;
    }
    if (*e->p == '-') { e->p++; return -ex_atom(e); }
    if (*e->p == '!') { e->p++; return !ex_atom(e); }

    if (isdigit((unsigned char)*e->p)) {
        char *end = NULL;
        long v = strtol(e->p, &end, 0);          /* 0x.. and decimal both work */
        e->p = end;
        return (int32_t)v;
    }
    if (*e->p == '$' || *e->p == '#' || isalpha((unsigned char)*e->p) || *e->p == '_') {
        char name[DS_DEF_NAME]; size_t n = 0;
        if (*e->p == '$' || *e->p == '#') e->p++;
        while ((isalnum((unsigned char)*e->p) || *e->p == '_') && n < sizeof(name) - 1)
            name[n++] = *e->p++;
        name[n] = 0;
        if (ieq(name, "TRUE"))  return 1;
        if (ieq(name, "FALSE")) return 0;
        /* OS constants, so a payload can write IF ($_OS == WINDOWS) */
        if (ieq(name, "WINDOWS")) return 0;
        if (ieq(name, "LINUX"))   return 1;
        if (ieq(name, "MAC") || ieq(name, "MACOS")) return 2;
        int32_t sysv;
        if (sys_var(e->ds, name, &sysv)) return sysv;
        int i = var_index(e->ds, name);
        if (i < 0) {
            /* Name the thing. "unknown variable" on a 300-line payload is an
             * invitation to guess; naming it turns it into a fix. A function
             * call used as a value gets its own message, because that is a
             * limitation of this interpreter rather than a mistake in the
             * payload - functions run as statements here, not as expressions. */
            const char *q2 = e->p;
            while (*q2 == ' ') q2++;
            if (*q2 == '(')
                snprintf(e->ds->errbuf, sizeof(e->ds->errbuf),
                         "%s() cannot be used as a value here", name);
            else
                snprintf(e->ds->errbuf, sizeof(e->ds->errbuf), "unknown variable $%s", name);
            fail(e->ds, e->ds->errbuf);
            return 0;
        }
        return e->ds->var[i].val;
    }
    fail(e->ds, "bad expression");
    return 0;
}

static int32_t ex_pow(ex_t *e)
{
    int32_t v = ex_atom(e);
    while (ex_eat(e, "^")) {
        int32_t r = ex_atom(e), acc = 1;
        for (int32_t i = 0; i < r && i < 31; i++) acc *= v;
        v = (r <= 0) ? 1 : acc;
    }
    return v;
}
static int32_t ex_mul(ex_t *e)
{
    int32_t v = ex_pow(e);
    for (;;) {
        if      (ex_eat(e, "*")) v *= ex_pow(e);
        else if (ex_eat(e, "/")) { int32_t d = ex_pow(e); if (!d) { fail(e->ds, "divide by zero"); return 0; } v /= d; }
        else if (ex_eat(e, "%")) { int32_t d = ex_pow(e); if (!d) { fail(e->ds, "modulo by zero"); return 0; } v %= d; }
        else return v;
    }
}
static int32_t ex_add(ex_t *e)
{
    int32_t v = ex_mul(e);
    for (;;) {
        if      (ex_eat(e, "+")) v += ex_mul(e);
        else if (ex_eat(e, "-")) v -= ex_mul(e);
        else return v;
    }
}
static int32_t ex_shift(ex_t *e)
{
    int32_t v = ex_add(e);
    for (;;) {
        if      (ex_eat(e, "<<")) v <<= (ex_add(e) & 31);
        else if (ex_eat(e, ">>")) v >>= (ex_add(e) & 31);
        else return v;
    }
}
static int32_t ex_cmp(ex_t *e)
{
    int32_t v = ex_shift(e);
    for (;;) {
        if      (ex_eat(e, "==")) v = (v == ex_shift(e));
        else if (ex_eat(e, "!=")) v = (v != ex_shift(e));
        else if (ex_eat(e, "<=")) v = (v <= ex_shift(e));
        else if (ex_eat(e, ">=")) v = (v >= ex_shift(e));
        else if (ex_eat(e, "<"))  v = (v <  ex_shift(e));
        else if (ex_eat(e, ">"))  v = (v >  ex_shift(e));
        else return v;
    }
}
static int32_t ex_bitand(ex_t *e)
{
    int32_t v = ex_cmp(e);
    while (ex_eat(e, "&")) v &= ex_cmp(e);
    return v;
}
static int32_t ex_bitor(ex_t *e)
{
    int32_t v = ex_bitand(e);
    while (ex_eat(e, "|")) v |= ex_bitand(e);
    return v;
}
static int32_t ex_and(ex_t *e)
{
    int32_t v = ex_bitor(e);
    while (ex_eat(e, "&&")) { int32_t r = ex_bitor(e); v = (v && r); }
    return v;
}
static int32_t ex_or(ex_t *e)
{
    int32_t v = ex_and(e);
    while (ex_eat(e, "||")) { int32_t r = ex_and(e); v = (v || r); }
    return v;
}

static int32_t eval(dscript_t *ds, const char *expr)
{
    ex_t e = { ds, expr };
    return ex_or(&e);
}

/* ------------------------------------------------------- structure scanning
 *
 * Skipping a block means finding the matching terminator at the SAME nesting
 * depth: an IF inside an IF must not close the outer one.
 */
/* Every spelling of the else half of a conditional.
 *
 * starts_with_kw() requires a delimiter after the keyword, and '_' is not one,
 * so "ELSE_IF" never matched "ELSE" - match_end() could not find it and the
 * ELSE handler did not recognise it either. Both spellings are in the keyword
 * table, so both have to work. */
static bool is_else_line(const char *l)
{
    if (starts_with_kw(l, "ELSE_IF")) return true;
    return starts_with_kw(l, "ELSE");
}

/* ...and the tail of an "ELSE IF <cond>" line, or NULL if it is a plain ELSE. */
static const char *else_if_cond(const char *l)
{
    if (starts_with_kw(l, "ELSE_IF")) return skip_ws(l + 7);
    if (starts_with_kw(l, "ELSE")) {
        const char *a = skip_ws(l + 4);
        if (starts_with_kw(a, "IF")) return skip_ws(a + 2);
    }
    return NULL;
}

static uint16_t match_end(dscript_t *ds, uint16_t from,
                          const char *open_kw, const char *close_kw,
                          const char *else_kw, bool stop_at_else)
{
    int depth = 0;
    char buf[DS_LINE_MAX];
    for (uint16_t i = from + 1; i < ds->nlines; i++) {
        get_line(ds, i, buf, sizeof(buf));
        const char *l = skip_ws(buf);
        if (starts_with_kw(l, open_kw))  { depth++; continue; }
        if (starts_with_kw(l, close_kw)) { if (depth == 0) return i; depth--; continue; }
        if (stop_at_else && depth == 0 && else_kw &&
            (else_kw[0] == 'E' && else_kw[1] == 'L' ? is_else_line(l)
                                                    : starts_with_kw(l, else_kw))) return i;
    }
    return 0xFFFF;
}

bool dscript_is_control(const char *line)
{
    const char *l = skip_ws(line);
    static const char *kw[] = {
        "VAR", "DEFINE", "IF", "ELSE", "ELSE_IF", "END_IF", "WHILE", "END_WHILE",
        "FUNCTION", "END_FUNCTION", "RETURN", NULL
    };
    for (int i = 0; kw[i]; i++) if (starts_with_kw(l, kw[i])) return true;
    return false;
}

bool dscript_is_consumed(const dscript_t *ds, const char *line)
{
    const char *l = skip_ws(line);
    if (dscript_is_control(l)) return true;

    /* an assignment: $name = ... (but not the comparison $name == ...) */
    if (*l == '$') {
        const char *r = l + 1;
        while (isalnum((unsigned char)*r) || *r == '_') r++;
        r = skip_ws(r);
        if (*r == '=' && r[1] != '=') return true;
    }

    /* a bare call to a function this script defines */
    char nm[20]; size_t n = 0;
    const char *q = l;
    while (*q && *q != ' ' && *q != '(' && n < sizeof(nm) - 1) nm[n++] = *q++;
    nm[n] = 0;
    const char *tail = skip_ws(q);
    if (*tail == 0 || (tail[0] == '(' && tail[1] == ')')) {
        for (int i = 0; i < ds->nfns; i++)
            if (ieq(ds->fn[i].name, nm)) return true;
    }
    return false;
}

/* ------------------------------------------------------------------ init */

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

dscript_t *dscript_alloc(void)
{
#ifdef ESP_PLATFORM
    /* External RAM: this struct is tens of kilobytes of line buffer, and
     * internal RAM is what the radio and the USB stack need. */
    dscript_t *p = heap_caps_calloc(1, sizeof(dscript_t), MALLOC_CAP_SPIRAM);
    if (!p) p = heap_caps_calloc(1, sizeof(dscript_t), MALLOC_CAP_DEFAULT);
    return p;
#else
    return (dscript_t *)calloc(1, sizeof(dscript_t));
#endif
}

bool dscript_init(dscript_t *ds, const char *text)
{
    memset(ds, 0, sizeof(*ds));
    if (!text) return false;
    /* Line offsets are uint16_t. Past 64 KB they wrap, and the interpreter
     * would then read confidently from the wrong places - a silent wrong
     * answer, which is the worst kind. Refuse with a reason instead. */
    size_t total = strlen(text);
    if (total > 0xFFFF) { ds->err = "payload is larger than 64 KB"; return false; }
    ds->text = text;

    /* index every line */
    const char *p = text;
    while (*p && ds->nlines < DS_MAX_LINES) {
        const char *start = p;
        while (*p && *p != '\n') p++;
        ds->line[ds->nlines].off = (uint16_t)(start - text);
        ds->line[ds->nlines].len = (uint16_t)(p - start);
        ds->nlines++;
        if (*p == '\n') p++;
    }
    if (*p) { ds->err = "payload is too long"; return false; }

    /* ---- structural validation, before anything runs ----
     *
     * An unterminated IF only shows itself at runtime if the branch is TAKEN,
     * and even then it merely runs off the end of the payload without
     * complaint - so a typo silently changes what a payload does. Checking the
     * structure up front turns that into a refusal naming the line, and lets
     * the linter report it before the device is ever armed. */
    {
        struct { const char *what; uint16_t line; } open[DS_MAX_DEPTH];
        int depth = 0;
        char b[DS_LINE_MAX];
        for (uint16_t i = 0; i < ds->nlines; i++) {
            get_line(ds, i, b, sizeof(b));
            const char *l = skip_ws(b);
            const char *opens = NULL, *closes = NULL;
            if      (starts_with_kw(l, "IF"))           opens  = "IF";
            else if (starts_with_kw(l, "WHILE"))        opens  = "WHILE";
            else if (starts_with_kw(l, "FUNCTION"))     opens  = "FUNCTION";
            else if (starts_with_kw(l, "END_IF"))       closes = "IF";
            else if (starts_with_kw(l, "END_WHILE"))    closes = "WHILE";
            else if (starts_with_kw(l, "END_FUNCTION")) closes = "FUNCTION";

            if (opens) {
                if (depth >= DS_MAX_DEPTH) {
                    ds->err = "blocks nested too deeply"; ds->err_line = (uint16_t)(i + 1); return false;
                }
                open[depth].what = opens; open[depth].line = (uint16_t)(i + 1); depth++;
            } else if (closes) {
                if (depth == 0) {
                    ds->err = (closes[0] == 'I') ? "END_IF without IF"
                            : (closes[0] == 'W') ? "END_WHILE without WHILE"
                                                 : "END_FUNCTION without FUNCTION";
                    ds->err_line = (uint16_t)(i + 1); return false;
                }
                depth--;
                if (strcmp(open[depth].what, closes) != 0) {
                    ds->err = "blocks closed in the wrong order";
                    ds->err_line = (uint16_t)(i + 1); return false;
                }
            }
        }
        if (depth > 0) {
            depth--;
            ds->err = (open[depth].what[0] == 'I') ? "IF without END_IF"
                    : (open[depth].what[0] == 'W') ? "WHILE without END_WHILE"
                                                   : "FUNCTION without END_FUNCTION";
            ds->err_line = open[depth].line;
            return false;
        }
    }

    /* pre-scan function definitions so a call can appear before the body */
    char buf[DS_LINE_MAX];
    for (uint16_t i = 0; i < ds->nlines; i++) {
        get_line(ds, i, buf, sizeof(buf));
        const char *l = skip_ws(buf);
        if (!starts_with_kw(l, "FUNCTION")) continue;
        const char *name = skip_ws(l + 8);
        if (!*name) { ds->err = "FUNCTION needs a name"; ds->err_line = i + 1; return false; }
        if (ds->nfns >= DS_MAX_FUNCS) { ds->err = "too many functions"; ds->err_line = i + 1; return false; }
        char nm[DS_DEF_NAME]; size_t n = 0;
        while (name[n] && name[n] != ' ' && name[n] != '(' && n < sizeof(nm) - 1) { nm[n] = name[n]; n++; }
        nm[n] = 0;
        snprintf(ds->fn[ds->nfns].name, sizeof(ds->fn[ds->nfns].name), "%s", nm);
        ds->fn[ds->nfns].line = i;
        ds->nfns++;
    }
    return true;
}

const char *dscript_error(const dscript_t *ds)   { return ds->err; }
uint16_t    dscript_error_line(const dscript_t *ds) { return ds->err_line; }

/* --------------------------------------------------------- $var expansion */

/* Replace every DEFINE name with its text, on whole-token boundaries so that
 * #FOO does not match inside #FOOBAR. Applied to each line before it is parsed,
 * which is exactly what the Ducky toolchain does at compile time. */
/* Returns false if the expanded line did not FIT.
 *
 * It used to truncate in silence, which on this device means typing half a
 * command into somebody's machine - a half-finished PowerShell line is not a
 * smaller version of the payload, it is a different one. The caller turns a
 * short buffer into a refusal with a line number instead. */
static bool expand_defines(dscript_t *ds, const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (const char *p = in; *p && o < cap - 1; ) {
        bool hit = false;
        for (int i = 0; i < ds->ndefs && !hit; i++) {
            size_t nl = strlen(ds->def[i].name);
            if (!nl || strncmp(p, ds->def[i].name, nl) != 0) continue;
            char after = p[nl];
            if (isalnum((unsigned char)after) || after == '_' || after == '-') continue;
            /* and it must start on a token boundary, not mid-word */
            if (p != in) {
                char before = p[-1];
                if (isalnum((unsigned char)before) || before == '_') continue;
            }
            int w = snprintf(out + o, cap - o, "%s", ds->def[i].val);
            if (w < 0 || (size_t)w >= cap - o) { out[cap - 1] = 0; return false; }
            o += (size_t)w;
            p += nl;
            hit = true;
        }
        if (!hit) out[o++] = *p++;
        if (*p && o >= cap - 1) { out[cap - 1] = 0; return false; }  /* ran out */
    }
    out[o < cap ? o : cap - 1] = 0;
    return true;
}

static bool expand_vars(dscript_t *ds, const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (const char *p = in; *p && o < cap - 1; ) {
        if ((*p == '$' || *p == '#') && (isalpha((unsigned char)p[1]) || p[1] == '_')) {
            char name[DS_DEF_NAME]; size_t n = 0;
            const char *q = p + 1;
            while ((isalnum((unsigned char)*q) || *q == '_') && n < sizeof(name) - 1) name[n++] = *q++;
            name[n] = 0;
            /* User variables first, then the $_ system variables.
             *
             * This only ever asked the user table, so IF ($_CAPSLOCK_ON) worked
             * - expressions resolve them separately - while
             * STRING caps lock: $_CAPSLOCK_ON typed the NAME. A payload that
             * reports what it found on the host is the whole point of the
             * return channel, and it printed placeholders instead. */
            int32_t val; bool have = false;
            int i = var_index(ds, name);
            if (i >= 0)                        { val = ds->var[i].val; have = true; }
            else if (sys_var(ds, name, &val))  { have = true; }
            if (have) {
                int w = snprintf(out + o, cap - o, "%ld", (long)val);
                if (w < 0 || (size_t)w >= cap - o) { out[cap - 1] = 0; return false; }
                o += (size_t)w;
                p = q;
                continue;
            }
        }
        out[o++] = *p++;
        if (*p && o >= cap - 1) { out[cap - 1] = 0; return false; }
    }
    out[o < cap ? o : cap - 1] = 0;
    return true;
}

/* ------------------------------------------------------------------ next */

const char *dscript_next(dscript_t *ds)
{
    char *buf = ds->work;   /* in the struct: 8 KB has no business on a task stack */

    while (!ds->err && ds->pc < ds->nlines) {
        if (++ds->steps > DS_MAX_STEPS) { fail(ds, "payload ran too long (loop with no end?)"); return NULL; }

        uint16_t here = ds->pc;
        get_line(ds, here, buf, DS_LINE_MAX);
        /* Macro expansion first, so everything below - conditions, delays,
         * text to type - sees the substituted text, exactly as the Ducky
         * toolchain would have produced it at compile time. */
        if (ds->ndefs) {
            if (!expand_defines(ds, buf, ds->out, sizeof(ds->out))) {
                fail(ds, "line is too long once its DEFINEs are substituted");
                return NULL;
            }
            memcpy(buf, ds->out, DS_LINE_MAX);
            buf[DS_LINE_MAX - 1] = 0;
        }
        const char *l = skip_ws(buf);
        ds->pc++;

        /* ---- inside a STRING / STRINGLN block ----
         * DuckyScript 3 lets a payload write several lines of text as a block
         * instead of repeating STRING on every one. Everything between the
         * opening keyword and its END_ is content, including blank lines and
         * anything that would otherwise look like a command. */
        if (ds->block) {
            const char *t = skip_ws(buf);
            if (starts_with_kw(t, "END_STRINGLN") || starts_with_kw(t, "END_STRING")) {
                ds->block = 0;
                continue;
            }
            snprintf(ds->out, sizeof(ds->out), "%s %s",
                     ds->block == 2 ? "STRINGLN" : "STRING", buf);
            return ds->out;
        }

        if (*l == 0) continue;

        /* ---- opening a block: the keyword alone on its line ---- */
        /* Blocks are opened by the bare keyword OR by the _BLOCK form the
         * official library actually uses, and both close with END_STRING(LN). */
        if (is_bare_kw(l, "STRINGLN") || is_bare_kw(l, "STRINGLN_BLOCK") ||
            is_bare_kw(l, "STRINGLN_BASH") || is_bare_kw(l, "STRINGLN_POWERSHELL")) { ds->block = 2; continue; }
        if (is_bare_kw(l, "STRING") || is_bare_kw(l, "STRING_BLOCK") ||
            is_bare_kw(l, "STRING_BASH") || is_bare_kw(l, "STRING_POWERSHELL")) { ds->block = 1; continue; }

        /* ---- EXTENSION ... END_EXTENSION ----
         * An extension is a named library block: variable initialisers and
         * FUNCTION definitions meant to be reused. On the Ducky the toolchain
         * splices them in at compile time; here they already sit in the file,
         * so the markers are transparent and the contents run where they are.
         * That gets the VARs initialised and the FUNCTIONs registered, which is
         * all the rest of the payload actually depends on. */
        if (starts_with_kw(l, "EXTENSION") || starts_with_kw(l, "END_EXTENSION")) continue;

        /* STAGE / END_STAGE are progress labels for the operator, not code. */
        if (starts_with_kw(l, "STAGE") || starts_with_kw(l, "END_STAGE")) continue;

        /* ---- IF_DEFINED_TRUE #CONST ... END_IF_DEFINED ----
         * Conditional inclusion on a DEFINE, used heavily by the official
         * library to build one payload with optional features. */
        if (starts_with_kw(l, "IF_DEFINED_TRUE") || starts_with_kw(l, "IF_DEFINED_FALSE")) {
            bool want_true = starts_with_kw(l, "IF_DEFINED_TRUE");
            const char *r = skip_ws(l + (want_true ? 15 : 16));
            if (*r == '#' || *r == '$') r++;
            char nm[16]; size_t k = 0;
            while ((isalnum((unsigned char)*r) || *r == '_') && k < sizeof(nm) - 1) nm[k++] = *r++;
            nm[k] = 0;
            int vi = var_index(ds, nm);
            bool defined_true = (vi >= 0) && (ds->var[vi].val != 0);
            if (defined_true != want_true) {
                /* skip to the matching END_IF_DEFINED, respecting nesting */
                int depth = 1;
                while (ds->pc < ds->nlines && depth > 0) {
                    char sk[DS_LINE_MAX];
                    get_line(ds, ds->pc++, sk, sizeof(sk));
                    const char *t = skip_ws(sk);
                    if (starts_with_kw(t, "IF_DEFINED_TRUE") || starts_with_kw(t, "IF_DEFINED_FALSE")) depth++;
                    else if (starts_with_kw(t, "END_IF_DEFINED")) depth--;
                }
            }
            continue;
        }
        if (starts_with_kw(l, "END_IF_DEFINED")) continue;

        /* ---- BUTTON_DEF ... END_BUTTON ----
         * Code the Ducky runs when its button is pressed mid-payload. This
         * device's button is the abort control while a payload runs, and
         * quietly rebinding it would take away the operator's stop. The block
         * is skipped rather than executed inline - which is the safe reading,
         * and stops the definition being typed as if it were the payload. */
        if (starts_with_kw(l, "BUTTON_DEF")) {
            while (ds->pc < ds->nlines) {
                char sk[DS_LINE_MAX];
                get_line(ds, ds->pc++, sk, sizeof(sk));
                if (starts_with_kw(skip_ws(sk), "END_BUTTON")) break;
            }
            continue;
        }
        if (starts_with_kw(l, "END_BUTTON")) continue;

        /* ---- INJECT_VAR $x : type the VALUE of a variable ---- */
        if (starts_with_kw(l, "INJECT_VAR")) {
            const char *r = skip_ws(l + 10);
            int32_t v = eval(ds, r);
            if (ds->err) return NULL;
            snprintf(ds->out, sizeof(ds->out), "STRING %ld", (long)v);
            return ds->out;
        }

        /* ---- payload control ---- */
        if (starts_with_kw(l, "RESTART_PAYLOAD")) {
            ds->pc = 0; ds->nret = 0; ds->nloop = 0; ds->block = 0;
            continue;
        }
        if (starts_with_kw(l, "STOP_PAYLOAD") || starts_with_kw(l, "EXIT")) { ds->pc = ds->nlines; return NULL; }
        if (starts_with_kw(l, "IF_NOT_DEFINED_TRUE") || starts_with_kw(l, "IF_NOT_DEFINED_FALSE")) continue;

        /* Comments are not executable lines. Handing them back made the
         * progress counter advance while nothing was being typed, and inflated
         * the total the UI divides by. REM_BLOCK is handled by the line parser
         * below; these two are cheap to drop here. */
        if (*l == '#') continue;
        if (starts_with_kw(l, "REM")) continue;

        /* ---- variables ---- */
        /* ---- DEFINE: a text macro ---- */
        if (starts_with_kw(l, "DEFINE")) {
            const char *r = skip_ws(l + 6);
            if (ds->ndefs < DS_MAX_DEFS) {
                size_t k = 0;
                /* the name is taken exactly as written, sigil included, since
                 * that is the token the payload later references */
                while (*r && *r != ' ' && *r != '\t' && k < DS_DEF_NAME - 1)
                    ds->def[ds->ndefs].name[k++] = *r++;
                ds->def[ds->ndefs].name[k] = 0;
                r = skip_ws(r);
                size_t v = 0;
                while (*r && *r != '\r' && v < DS_DEF_VAL - 1) ds->def[ds->ndefs].val[v++] = *r++;
                while (v > 0 && (ds->def[ds->ndefs].val[v-1] == ' ' ||
                                 ds->def[ds->ndefs].val[v-1] == '\t')) v--;
                ds->def[ds->ndefs].val[v] = 0;
                if (k) ds->ndefs++;
            }
            continue;
        }

        if (starts_with_kw(l, "VAR")) {
            const char *r = skip_ws(l + 3);
            if (*r == '$' || *r == '#') r++;
            char name[DS_DEF_NAME]; size_t n = 0;
            while ((isalnum((unsigned char)*r) || *r == '_') && n < sizeof(name) - 1) name[n++] = *r++;
            name[n] = 0;
            r = skip_ws(r);
            if (*r == '=') r++;
            /* "VAR $x = FUNC()" must capture the return value exactly as
             * "$x = FUNC()" does. Only the second form was handled, so a
             * declaration that called a function silently evaluated to
             * nothing - which is how the T3 test caught it. */
            {
                const char *rhs = skip_ws(r);
                char cn[DS_DEF_NAME]; size_t cl = 0;
                const char *cq = rhs;
                while ((isalnum((unsigned char)*cq) || *cq == '_') && cl < sizeof(cn) - 1) cn[cl++] = *cq++;
                cn[cl] = 0;
                const char *ct = skip_ws(cq);
                if (cl && ct[0] == '(' && ct[1] == ')') {
                    int fi = -1;
                    for (int i = 0; i < ds->nfns; i++) if (ieq(ds->fn[i].name, cn)) { fi = i; break; }
                    if (fi >= 0) {
                        if (ds->nret >= DS_MAX_DEPTH) { fail(ds, "functions nested too deeply"); return NULL; }
                        var_set(ds, name, 0);            /* declare it before the call */
                        snprintf(ds->ret_var[ds->nret], DS_DEF_NAME, "%s", name);
                        ds->ret[ds->nret++] = ds->pc;
                        ds->pc = (uint16_t)(ds->fn[fi].line + 1);
                        continue;
                    }
                }
            }
            int32_t v = eval(ds, r);
            if (!ds->err) var_set(ds, name, v);
            continue;
        }

        /* ---- assignment to an existing variable:  $x = $x + 1 ---- */
        if (*l == '$') {
            const char *r = l + 1;
            char name[DS_DEF_NAME]; size_t n = 0;
            while ((isalnum((unsigned char)*r) || *r == '_') && n < sizeof(name) - 1) name[n++] = *r++;
            name[n] = 0;
            const char *after = skip_ws(r);
            if (*after == '=' && after[1] != '=') {
                /* If the right-hand side is a call to a function this payload
                 * defines, run it as a statement and let its RETURN assign the
                 * result. That is the shape the official payloads use, and it
                 * avoids evaluating a function inside an expression - which
                 * would mean running code, possibly typing, mid-expression. */
                const char *rhs = skip_ws(after + 1);
                char cn[DS_DEF_NAME]; size_t cnl = 0;
                const char *cq = rhs;
                while ((isalnum((unsigned char)*cq) || *cq == '_') && cnl < sizeof(cn) - 1) cn[cnl++] = *cq++;
                cn[cnl] = 0;
                const char *ctail = skip_ws(cq);
                if (cnl && ctail[0] == '(' && ctail[1] == ')') {
                    int fi = -1;
                    for (int i = 0; i < ds->nfns; i++) if (ieq(ds->fn[i].name, cn)) { fi = i; break; }
                    if (fi >= 0) {
                        if (ds->nret >= DS_MAX_DEPTH) { fail(ds, "functions nested too deeply"); return NULL; }
                        /* remember WHERE the result goes, then call */
                        snprintf(ds->ret_var[ds->nret], DS_DEF_NAME, "%s", name);
                        ds->ret[ds->nret++] = ds->pc;
                        ds->pc = (uint16_t)(ds->fn[fi].line + 1);
                        continue;
                    }
                }
                int32_t v = eval(ds, after + 1);
                if (!ds->err) var_set(ds, name, v);
                continue;
            }
        }

        /* ---- conditionals ---- */
        if (starts_with_kw(l, "IF")) {
            const char *cond = skip_ws(l + 2);
            /* tolerate the documented "IF <cond> THEN" form */
            char c[DS_LINE_MAX];
            snprintf(c, sizeof(c), "%s", cond);
            char *then = strstr(c, "THEN");
            if (!then) then = strstr(c, "then");
            if (then) *then = 0;
            int32_t v = eval(ds, c);
            if (ds->err) return NULL;
            /* Walk the ELSE IF chain.
             *
             * A false condition jumped to the line AFTER the next ELSE - and
             * for "ELSE IF (...)" that line is its BODY. The condition was
             * never evaluated, so the branch ran unconditionally and every
             * value past the second one took the second branch. Land ON each
             * ELSE IF and actually test it. */
            uint16_t at = here;
            while (!v) {
                uint16_t j = match_end(ds, at, "IF", "END_IF", "ELSE", true);
                if (j == 0xFFFF) { fail(ds, "IF without END_IF"); return NULL; }
                get_line(ds, j, buf, DS_LINE_MAX)   /* buf is a POINTER: sizeof is 8 */;
                const char *cond2 = else_if_cond(skip_ws(buf));
                if (!cond2) {                    /* a plain ELSE, or END_IF */
                    ds->pc = (uint16_t)(j + 1);
                    break;
                }
                char c2[DS_LINE_MAX];
                snprintf(c2, sizeof(c2), "%s", cond2);
                char *t2 = strstr(c2, "THEN");
                if (!t2) t2 = strstr(c2, "then");
                if (t2) *t2 = 0;
                v = eval(ds, c2);
                if (ds->err) return NULL;
                at = j;
                if (v) ds->pc = (uint16_t)(j + 1);   /* this branch is taken */
            }
            continue;
        }
        if (is_else_line(l)) {
            /* reached only by falling out of a taken IF branch: skip the else */
            uint16_t j = match_end(ds, here, "IF", "END_IF", NULL, false);
            if (j == 0xFFFF) { fail(ds, "ELSE without END_IF"); return NULL; }
            ds->pc = (uint16_t)(j + 1);
            continue;
        }
        if (starts_with_kw(l, "END_IF")) continue;

        /* ---- loops ---- */
        if (starts_with_kw(l, "WHILE")) {
            int32_t v = eval(ds, skip_ws(l + 5));
            if (ds->err) return NULL;
            if (v) {
                if (ds->nloop >= DS_MAX_DEPTH) { fail(ds, "loops nested too deeply"); return NULL; }
                ds->loop[ds->nloop++] = here;
            } else {
                uint16_t j = match_end(ds, here, "WHILE", "END_WHILE", NULL, false);
                if (j == 0xFFFF) { fail(ds, "WHILE without END_WHILE"); return NULL; }
                ds->pc = (uint16_t)(j + 1);
            }
            continue;
        }
        if (starts_with_kw(l, "END_WHILE")) {
            if (ds->nloop == 0) { fail(ds, "END_WHILE without WHILE"); return NULL; }
            ds->pc = ds->loop[--ds->nloop];      /* re-test the condition */
            continue;
        }

        /* ---- functions ---- */
        if (starts_with_kw(l, "FUNCTION")) {
            uint16_t j = match_end(ds, here, "FUNCTION", "END_FUNCTION", NULL, false);
            if (j == 0xFFFF) { fail(ds, "FUNCTION without END_FUNCTION"); return NULL; }
            ds->pc = (uint16_t)(j + 1);          /* definitions do not run inline */
            continue;
        }
        if (starts_with_kw(l, "END_FUNCTION") || starts_with_kw(l, "RETURN")) {
            if (ds->nret == 0) { ds->pc = ds->nlines; return NULL; }   /* RETURN at top level ends the payload */
            /* "RETURN <expr>" hands a value back to "$X = FUNC()". */
            if (starts_with_kw(l, "RETURN")) {
                const char *r = skip_ws(l + 6);
                if (*r && ds->ret_var[ds->nret - 1][0]) {
                    int32_t v = eval(ds, r);
                    if (!ds->err) var_set(ds, ds->ret_var[ds->nret - 1], v);
                }
            }
            ds->nret--;
            ds->ret_var[ds->nret][0] = 0;
            ds->pc = ds->ret[ds->nret];
            continue;
        }
        {   /* a bare name that matches a FUNCTION is a call */
            char nm[DS_DEF_NAME]; size_t n = 0;
            const char *q = l;
            while (*q && *q != ' ' && *q != '(' && n < sizeof(nm) - 1) nm[n++] = *q++;
            nm[n] = 0;
            const char *tail = skip_ws(q);
            if (*tail == 0 || (tail[0] == '(' && tail[1] == ')')) {
                for (int i = 0; i < ds->nfns; i++) {
                    if (!ieq(ds->fn[i].name, nm)) continue;
                    if (ds->nret >= DS_MAX_DEPTH) { fail(ds, "functions nested too deeply"); return NULL; }
                    ds->ret_var[ds->nret][0] = 0;
                    ds->ret[ds->nret++] = ds->pc;
                    ds->pc = (uint16_t)(ds->fn[i].line + 1);
                    goto continue_outer;
                }
            }
        }

        /* ---- anything else types something: hand it back, expanded ---- */
        if (!expand_vars(ds, l, ds->out, sizeof(ds->out))) {
            fail(ds, "line is too long once its variables are substituted");
            return NULL;
        }
        return ds->out;

    continue_outer:
        continue;
    }
    return NULL;
}
