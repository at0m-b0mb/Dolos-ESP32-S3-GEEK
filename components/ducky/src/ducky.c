#include "ducky.h"
#include "hid_keys.h"
#include "layout.h"
#include "unicode.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>

static int kw(const char *tok, const char *word);  /* case-insensitive equals */

/* Consumer-control (media) usage codes, HID consumer page 0x0C. */
static uint16_t media_usage(const char *n)
{
    if (kw(n,"PLAY")||kw(n,"PAUSE")||kw(n,"PLAYPAUSE")) return 0xCD;
    if (kw(n,"NEXT")) return 0xB5;
    if (kw(n,"PREV")||kw(n,"PREVIOUS")) return 0xB6;
    if (kw(n,"STOP")) return 0xB7;
    if (kw(n,"MUTE")) return 0xE2;
    if (kw(n,"VOLUP")||kw(n,"VOLUMEUP")) return 0xE9;
    if (kw(n,"VOLDOWN")||kw(n,"VOLUMEDOWN")) return 0xEA;
    return 0;
}
static uint8_t mouse_button(const char *n)
{
    if (kw(n,"LEFT")||kw(n,"L")) return 1;
    if (kw(n,"RIGHT")||kw(n,"R")) return 2;
    if (kw(n,"MIDDLE")||kw(n,"M")) return 4;
    return 0;
}
static int8_t clamp127(int v){ return (int8_t)(v>127?127:(v<-127?-127:v)); }

void ducky_state_init(ducky_state_t *st)
{
    st->default_delay_ms = 0;
    st->last_cmd[0] = 0;
    st->repeat = 0;
    st->layout = LAYOUT_US;
    st->target_os = OS_WINDOWS;
}

static int kw(const char *tok, const char *word)  /* case-insensitive equals */
{
    for (; *tok && *word; tok++, word++)
        if (toupper((unsigned char)*tok) != toupper((unsigned char)*word)) return 0;
    return *tok == 0 && *word == 0;
}

/* Emit one KEY action per character of `s`. Returns count (<= max). */
static int emit_string(const char *s, kb_layout_t layout, target_os_t os,
                       ducky_action_t *out, int max)
{
    int n = 0; const char *p = s;
    while (n < max) {
        uint32_t cp; int adv = utf8_next(&p, &cp);
        if (adv == 0) break;
        if (cp < 0x80) {                       /* ASCII: use the target layout */
            uint8_t k, m;
            if (!hid_from_ascii_layout((char)cp, layout, &k, &m)) continue;
            memset(&out[n], 0, sizeof(out[n]));
            out[n].kind = DUCKY_KEY; out[n].key = k; out[n].mods = m; n++;
        } else {                               /* non-ASCII: OS Unicode method */
            int adds = unicode_seq(cp, os, out + n, max - n);
            if (adds == 0) break;
            n += adds;
        }
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
    if (kw(cmd, "UNICODE")) {
        const char *h = rest;
        if ((h[0] == 'U' || h[0] == 'u') && h[1] == '+') h += 2;
        uint32_t cp = (uint32_t)strtoul(h, NULL, 16);
        return cp ? unicode_seq(cp, st->target_os, out, max) : 0;
    }
    if (kw(cmd, "STRING"))   return emit_string(rest, st->layout, st->target_os, out, max);
    if (kw(cmd, "STRINGLN")) {
        int k = emit_string(rest, st->layout, st->target_os, out, max);
        if (k < max) { out[k].kind = DUCKY_KEY; out[k].key = HID_KEY_ENTER;
                       out[k].mods = 0; out[k].delay_ms = 0; k++; }
        return k;
    }

    if (kw(cmd, "MOUSEMOVE")) {
        int x = 0, y = 0; sscanf(rest, "%d %d", &x, &y);
        out[0].kind = DUCKY_MOUSE; out[0].mx = clamp127(x); out[0].my = clamp127(y);
        out[0].wheel = 0; out[0].buttons = 0; out[0].key = 0; out[0].mods = 0;
        return 1;
    }
    if (kw(cmd, "MOUSECLICK") || kw(cmd, "MOUSEBUTTON")) {
        out[0].kind = DUCKY_MOUSE; out[0].buttons = mouse_button(rest);
        out[0].mx = out[0].my = out[0].wheel = 0; out[0].key = 0; out[0].mods = 0;
        return out[0].buttons ? 1 : 0;
    }
    if (kw(cmd, "MOUSEWHEEL") || kw(cmd, "MOUSESCROLL")) {
        out[0].kind = DUCKY_MOUSE; out[0].wheel = clamp127(atoi(rest));
        out[0].mx = out[0].my = 0; out[0].buttons = 0; out[0].key = 0; out[0].mods = 0;
        return 1;
    }
    if (kw(cmd, "MEDIA") || kw(cmd, "CONSUMER")) {
        uint16_t u = media_usage(rest);
        if (!u) return 0;
        out[0].kind = DUCKY_CONSUMER; out[0].consumer = u; out[0].key = 0; out[0].mods = 0;
        return 1;
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
                else if (p == 1 && hid_from_ascii_layout(tok[0], st->layout, &k, NULL)) { key = k; have_key = true; }
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
