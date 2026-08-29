#include "ducky.h"
#include "hid_keys.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

void ducky_state_init(ducky_state_t *st)
{
    st->default_delay_ms = 0;
    st->last_cmd[0] = 0;
    st->repeat = 0;
}

static int kw(const char *tok, const char *word)  /* case-insensitive equals */
{
    for (; *tok && *word; tok++, word++)
        if (toupper((unsigned char)*tok) != toupper((unsigned char)*word)) return 0;
    return *tok == 0 && *word == 0;
}

/* Emit one KEY action per character of `s`. Returns count (<= max). */
static int emit_string(const char *s, ducky_action_t *out, int max)
{
    int n = 0;
    for (; *s && n < max; s++) {
        uint8_t k, m;
        if (!hid_from_ascii(*s, &k, &m)) continue;   /* skip unmapped bytes */
        out[n].kind = DUCKY_KEY; out[n].key = k; out[n].mods = m; out[n].delay_ms = 0;
        n++;
    }
    return n;
}

int ducky_parse_line(ducky_state_t *st, const char *line,
                     ducky_action_t *out, int max)
{
    st->repeat = 0;
    if (!line || max <= 0) return 0;

    /* trim leading whitespace + trailing CR/LF into a working buffer */
    char buf[224];
    while (*line == ' ' || *line == '\t') line++;
    size_t n = 0;
    while (line[n] && line[n] != '\r' && line[n] != '\n' && n < sizeof(buf) - 1) {
        buf[n] = line[n]; n++;
    }
    buf[n] = 0;
    while (n > 0 && (buf[n-1] == ' ' || buf[n-1] == '\t')) buf[--n] = 0;
    if (n == 0) return 0;

    /* first token = command */
    char cmd[24]; size_t c = 0;
    while (buf[c] && buf[c] != ' ' && c < sizeof(cmd) - 1) { cmd[c] = buf[c]; c++; }
    cmd[c] = 0;
    const char *rest = buf + c;
    if (*rest == ' ') rest++;                    /* content begins after one space */

    if (kw(cmd, "REM") || cmd[0] == '#') return 0;            /* comment */
    if (kw(cmd, "REPEAT")) { int r = atoi(rest); st->repeat = r > 0 ? r : 0; return 0; }
    if (kw(cmd, "DEFAULTDELAY") || kw(cmd, "DEFAULT_DELAY")) {
        st->default_delay_ms = (uint32_t)atoi(rest); return 0;
    }

    /* everything below is a real command; remember it for REPEAT */
    strncpy(st->last_cmd, buf, sizeof(st->last_cmd) - 1);
    st->last_cmd[sizeof(st->last_cmd) - 1] = 0;

    if (kw(cmd, "DELAY")) {
        out[0].kind = DUCKY_DELAY; out[0].delay_ms = (uint32_t)atoi(rest);
        out[0].key = 0; out[0].mods = 0;
        return 1;
    }
    if (kw(cmd, "STRING"))   return emit_string(rest, out, max);
    if (kw(cmd, "STRINGLN")) {
        int k = emit_string(rest, out, max);
        if (k < max) { out[k].kind = DUCKY_KEY; out[k].key = HID_KEY_ENTER;
                       out[k].mods = 0; out[k].delay_ms = 0; k++; }
        return k;
    }

    /* Otherwise: a key chord. Walk every token; modifiers OR together, the last
     * named key or single character is the key. "GUI r", "CTRL ALT DELETE". */
    uint8_t mods = 0, key = 0; bool have_key = false, invalid = false;
    char tok[24]; size_t p = 0;
    const char *q = buf;
    for (;;) {
        if (*q == ' ' || *q == 0) {
            if (p > 0) {
                tok[p] = 0;
                uint8_t m, k;
                if (hid_modifier(tok, &m))        mods |= m;
                else if (hid_named_key(tok, &k)) { key = k; have_key = true; }
                else if (p == 1 && hid_from_ascii(tok[0], &k, NULL)) { key = k; have_key = true; }
                else invalid = true;
                p = 0;
            }
            if (*q == 0) break;
        } else if (p < sizeof(tok) - 1) {
            tok[p++] = *q;
        }
        q++;
    }
    if (invalid) return 0;
    if (!have_key && mods == 0) return 0;
    out[0].kind = DUCKY_KEY; out[0].mods = mods; out[0].key = key; out[0].delay_ms = 0;
    return 1;
}
