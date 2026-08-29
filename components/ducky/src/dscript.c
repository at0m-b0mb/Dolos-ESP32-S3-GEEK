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
static bool starts_with_kw(const char *line, const char *kw)
{
    size_t n = strlen(kw);
    for (size_t i = 0; i < n; i++)
        if (toupper((unsigned char)line[i]) != toupper((unsigned char)kw[i])) return false;
    return line[n] == 0 || line[n] == ' ' || line[n] == '\t';
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

bool dscript_get(const dscript_t *ds, const char *name, int32_t *out)
{
    if (*name == '$' || *name == '#') name++;
    int i = var_index(ds, name);
    if (i < 0) return false;
    if (out) *out = ds->var[i].val;
    return true;
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
        char name[16]; size_t n = 0;
        if (*e->p == '$' || *e->p == '#') e->p++;
        while ((isalnum((unsigned char)*e->p) || *e->p == '_') && n < sizeof(name) - 1)
            name[n++] = *e->p++;
        name[n] = 0;
        if (ieq(name, "TRUE"))  return 1;
        if (ieq(name, "FALSE")) return 0;
        int i = var_index(e->ds, name);
        if (i < 0) { fail(e->ds, "unknown variable"); return 0; }
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
        if (stop_at_else && depth == 0 && else_kw && starts_with_kw(l, else_kw)) return i;
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

bool dscript_init(dscript_t *ds, const char *text)
{
    memset(ds, 0, sizeof(*ds));
    if (!text) return false;
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
        char nm[20]; size_t n = 0;
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

static void expand_vars(dscript_t *ds, const char *in, char *out, size_t cap)
{
    size_t o = 0;
    for (const char *p = in; *p && o < cap - 1; ) {
        if (*p == '$' && (isalpha((unsigned char)p[1]) || p[1] == '_')) {
            char name[16]; size_t n = 0;
            const char *q = p + 1;
            while ((isalnum((unsigned char)*q) || *q == '_') && n < sizeof(name) - 1) name[n++] = *q++;
            name[n] = 0;
            int i = var_index(ds, name);
            if (i >= 0) {
                o += (size_t)snprintf(out + o, cap - o, "%ld", (long)ds->var[i].val);
                p = q;
                continue;
            }
        }
        out[o++] = *p++;
    }
    out[o < cap ? o : cap - 1] = 0;
}

/* ------------------------------------------------------------------ next */

const char *dscript_next(dscript_t *ds)
{
    char buf[DS_LINE_MAX];

    while (!ds->err && ds->pc < ds->nlines) {
        if (++ds->steps > DS_MAX_STEPS) { fail(ds, "payload ran too long (loop with no end?)"); return NULL; }

        uint16_t here = ds->pc;
        get_line(ds, here, buf, sizeof(buf));
        const char *l = skip_ws(buf);
        ds->pc++;

        if (*l == 0) continue;

        /* Comments are not executable lines. Handing them back made the
         * progress counter advance while nothing was being typed, and inflated
         * the total the UI divides by. REM_BLOCK is handled by the line parser
         * below; these two are cheap to drop here. */
        if (*l == '#') continue;
        if (starts_with_kw(l, "REM")) continue;

        /* ---- variables ---- */
        if (starts_with_kw(l, "VAR") || starts_with_kw(l, "DEFINE")) {
            const char *r = skip_ws(l + (starts_with_kw(l, "VAR") ? 3 : 6));
            if (*r == '$' || *r == '#') r++;
            char name[16]; size_t n = 0;
            while ((isalnum((unsigned char)*r) || *r == '_') && n < sizeof(name) - 1) name[n++] = *r++;
            name[n] = 0;
            r = skip_ws(r);
            if (*r == '=') r++;
            int32_t v = eval(ds, r);
            if (!ds->err) var_set(ds, name, v);
            continue;
        }

        /* ---- assignment to an existing variable:  $x = $x + 1 ---- */
        if (*l == '$') {
            const char *r = l + 1;
            char name[16]; size_t n = 0;
            while ((isalnum((unsigned char)*r) || *r == '_') && n < sizeof(name) - 1) name[n++] = *r++;
            name[n] = 0;
            const char *after = skip_ws(r);
            if (*after == '=' && after[1] != '=') {
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
            if (!v) {
                uint16_t j = match_end(ds, here, "IF", "END_IF", "ELSE", true);
                if (j == 0xFFFF) { fail(ds, "IF without END_IF"); return NULL; }
                get_line(ds, j, buf, sizeof(buf));
                ds->pc = (uint16_t)(j + 1);      /* land after ELSE or END_IF */
            }
            continue;
        }
        if (starts_with_kw(l, "ELSE")) {
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
            ds->pc = ds->ret[--ds->nret];
            continue;
        }
        {   /* a bare name that matches a FUNCTION is a call */
            char nm[20]; size_t n = 0;
            const char *q = l;
            while (*q && *q != ' ' && *q != '(' && n < sizeof(nm) - 1) nm[n++] = *q++;
            nm[n] = 0;
            const char *tail = skip_ws(q);
            if (*tail == 0 || (tail[0] == '(' && tail[1] == ')')) {
                for (int i = 0; i < ds->nfns; i++) {
                    if (!ieq(ds->fn[i].name, nm)) continue;
                    if (ds->nret >= DS_MAX_DEPTH) { fail(ds, "functions nested too deeply"); return NULL; }
                    ds->ret[ds->nret++] = ds->pc;
                    ds->pc = (uint16_t)(ds->fn[i].line + 1);
                    goto continue_outer;
                }
            }
        }

        /* ---- anything else types something: hand it back, expanded ---- */
        expand_vars(ds, l, ds->out, sizeof(ds->out));
        return ds->out;

    continue_outer:
        continue;
    }
    return NULL;
}
