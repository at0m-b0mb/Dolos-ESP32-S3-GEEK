#include "ducky.h"
#include "hid_keys.h"
#include "layout.h"
#include "unicode.h"
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>

static int kw(const char *tok, const char *word);  /* case-insensitive equals */

/* Case-insensitive prefix test. Hand-rolled because the POSIX case-insensitive
 * compare is not ISO C: macOS exposes it regardless, glibc hides it under
 * -std=c11, and the difference only shows up in CI. */
static int kw_prefix(const char *s, const char *pfx, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (!s[i] || !pfx[i]) return 0;
        if (toupper((unsigned char)s[i]) != toupper((unsigned char)pfx[i])) return 0;
    }
    return 1;
}

/* Copy at most cap-1 bytes and always terminate. snprintf("%s") with a source
 * that can be far longer than the destination makes GCC warn about truncation
 * it cannot prove is intentional - and here it very much is. */
static void copy_bounded(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (!cap) return;
    while (src && src[i] && i < cap - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

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

/* xorshift: small, deterministic for tests, reseeded from hardware on device */
static uint32_t rng_next(ducky_state_t *st)
{
    uint32_t x = st->rng_state ? st->rng_state : 0x2545F491u;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    st->rng_state = x;
    return x;
}

/* RANDOM_* pick one character from a class and type it. */
static int emit_random(ducky_state_t *st, const char *cmd, ducky_action_t *out, int max)
{
    static const char LOWER[] = "abcdefghijklmnopqrstuvwxyz";
    static const char UPPER[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static const char DIGIT[] = "0123456789";
    static const char SPECL[] = "!@#$%^&*()-_=+[]{};:,.<>/?";
    const char *set = NULL;
    if      (kw(cmd, "RANDOM_LOWERCASE_LETTER")) set = LOWER;
    else if (kw(cmd, "RANDOM_UPPERCASE_LETTER")) set = UPPER;
    else if (kw(cmd, "RANDOM_NUMBER"))           set = DIGIT;
    else if (kw(cmd, "RANDOM_SPECIAL"))          set = SPECL;
    else if (kw(cmd, "RANDOM_LETTER")) {
        set = (rng_next(st) & 1) ? LOWER : UPPER;
    } else if (kw(cmd, "RANDOM_CHAR")) {
        switch (rng_next(st) & 3) {
            case 0: set = LOWER; break; case 1: set = UPPER; break;
            case 2: set = DIGIT; break; default: set = SPECL; break;
        }
    } else return -1;                    /* not a RANDOM command */
    if (max < 1) return 0;
    size_t len = strlen(set);
    char c = set[rng_next(st) % len];
    uint8_t k, m;
    if (!hid_from_ascii_layout(c, st->layout, &k, &m)) return 0;
    memset(&out[0], 0, sizeof(out[0]));
    out[0].kind = DUCKY_KEY; out[0].key = k; out[0].mods = m;
    return 1;
}

void ducky_state_init(ducky_state_t *st)
{
    st->default_delay_ms = 0;
    st->last_cmd[0] = 0;
    st->repeat = 0;
    st->layout = LAYOUT_US;
    st->target_os = OS_WINDOWS;
    st->string_delay_ms = 0;
    st->in_rem_block = false;
    st->rng_state = 0x2545F491u;
    st->pending = NULL;
    st->pending_ln = false;
}

static int emit_string(const char *s, kb_layout_t layout, target_os_t os,
                       uint32_t char_delay, ducky_action_t *out, int max,
                       const char **endp);

int ducky_continue(ducky_state_t *st, ducky_action_t *out, int max)
{
    if (!st || !st->pending || !out || max <= 0) return 0;
    const char *end = st->pending;
    int k = emit_string(st->pending, st->layout, st->target_os,
                        st->string_delay_ms, out, max, &end);
    if (end == st->pending) {        /* no progress: this character is untypable */
        st->pending = NULL; st->pending_ln = false;
        return k;
    }
    if (*end) { st->pending = end; return k; }   /* still more to come */
    st->pending = NULL;
    if (st->pending_ln && k < max) { memset(&out[k], 0, sizeof(out[k]));
                                     out[k].kind = DUCKY_KEY; out[k].key = HID_KEY_ENTER; k++; }
    st->pending_ln = false;
    return k;
}

static int kw(const char *tok, const char *word)  /* case-insensitive equals */
{
    for (; *tok && *word; tok++, word++)
        if (toupper((unsigned char)*tok) != toupper((unsigned char)*word)) return 0;
    return *tok == 0 && *word == 0;
}

/* Emit one KEY action per character of `s`. Returns count (<= max). */
static int emit_string(const char *s, kb_layout_t layout, target_os_t os,
                       uint32_t char_delay, ducky_action_t *out, int max,
                       const char **endp)
{
    int n = 0; const char *p = s; bool first = true;
    while (n < max) {
        /* Where this character began. When it will not fit, the cursor is put
         * back here so the next pass retypes it instead of losing it. */
        const char *start = p;
        uint32_t cp; int adv = utf8_next(&p, &cp);
        if (adv == 0) break;
        /* STRINGDELAY paces the characters of this line without slowing the
         * whole payload down: some hosts swallow fast typing only in certain
         * windows (a freshly opened dialog, a remote session), and a global
         * speed change is a blunt instrument for that. */
        if (!first && char_delay) {
            if (n + 1 >= max) { p = start; break; }   /* room for delay AND key */
            memset(&out[n], 0, sizeof(out[n]));
            out[n].kind = DUCKY_DELAY; out[n].delay_ms = char_delay; n++;
        }
        if (n >= max) { p = start; break; }
        if (cp < 0x80) {                       /* ASCII: use the target layout */
            uint8_t k, m;
            if (!hid_from_ascii_layout((char)cp, layout, &k, &m)) continue;
            memset(&out[n], 0, sizeof(out[n]));
            out[n].kind = DUCKY_KEY; out[n].key = k; out[n].mods = m; n++;
        } else {
            /* If the character is a KEY on the target layout, press it. One
             * report instead of seven, and none of the operating-system
             * requirements the Unicode method carries. */
            uint8_t uk, um, dk, dm, bk, bm;
            if (layout_utf8_key(layout, cp, &uk, &um)) {
                memset(&out[n], 0, sizeof(out[n]));
                out[n].kind = DUCKY_KEY; out[n].key = uk; out[n].mods = um; n++;
            } else if (layout_utf8_combo(layout, cp, &dk, &dm, &bk, &bm) && n + 1 < max) {
                /* accent key, then the letter: two keystrokes, still far
                 * cheaper and more portable than the OS Unicode method */
                memset(&out[n], 0, sizeof(out[n]));
                out[n].kind = DUCKY_KEY; out[n].key = dk; out[n].mods = dm; n++;
                memset(&out[n], 0, sizeof(out[n]));
                out[n].kind = DUCKY_KEY; out[n].key = bk; out[n].mods = bm; n++;
            } else {                           /* otherwise: OS Unicode method */
                /* On macOS the Option sequences come first: they work on the
                 * keyboard the machine already has, where the hex method does
                 * not. */
                int adds = 0;
                if (os == OS_MAC) adds = mac_option_seq(cp, out + n, max - n);
                if (adds == 0)    adds = unicode_seq(cp, os, out + n, max - n);
                if (adds == 0) { p = start; break; }
                n += adds;
            }
        }
        first = false;
    }
    if (endp) *endp = p;
    return n;
}

uint8_t ducky_apply_caps(uint8_t key, uint8_t mods, uint8_t leds)
{
    /* Only the letter keys are affected: Caps Lock does not shift digits or
     * punctuation, so inverting those would corrupt the very symbols payloads
     * depend on. */
    if (key < HID_KEY_A || key > (HID_KEY_A + 25)) return mods;
    if (!(leds & HID_LED_CAPSLOCK))                return mods;
    return (uint8_t)(mods ^ HID_MOD_LSHIFT);
}

int ducky_parse_line(ducky_state_t *st, const char *line,
                     ducky_action_t *out, int max)
{
    st->repeat = 0;
    st->pending = NULL; st->pending_ln = false;   /* scratch is about to be reused */
    if (!line || max <= 0) return 0;

    /* trim leading whitespace + trailing CR/LF into a working buffer */
    char *buf = st->scratch;
    while (*line == ' ' || *line == '\t') line++;
    size_t n = 0;
    while (line[n] && line[n] != '\r' && line[n] != '\n' && n < 8191) {
        buf[n] = line[n]; n++;
    }
    buf[n] = 0;
    while (n > 0 && (buf[n-1] == ' ' || buf[n-1] == '\t')) buf[--n] = 0;
    if (n == 0) return 0;

    /* first token = command */
    /* Long enough for the longest real command name:
     * RESTORE_HOST_KEYBOARD_LOCK_STATE is 32 characters. A 24-byte buffer
     * silently truncated it to something that matched nothing, so the command
     * was reported as unknown while looking perfectly correct in the file. */
    char cmd[40]; size_t c = 0;
    while (buf[c] && buf[c] != ' ' && c < sizeof(cmd) - 1) { cmd[c] = buf[c]; c++; }
    cmd[c] = 0;
    const char *rest = buf + c;
    if (*rest == ' ') rest++;                    /* content begins after one space */

    /* REM_BLOCK ... END_REM: everything between is commentary. Checked before
     * anything else so a block can legally contain command-looking lines. */
    if (st->in_rem_block) {
        if (kw(cmd, "END_REM")) st->in_rem_block = false;
        return 0;
    }
    if (kw(cmd, "REM_BLOCK")) { st->in_rem_block = true; return 0; }
    /* "REM", and also "REM:" / "REM<" - people punctuate their comments, and
     * a remark that fails to parse as a remark is a poor showing. */
    if (kw(cmd, "REM") || cmd[0] == '#') return 0;
    if ((cmd[0] == 'R' || cmd[0] == 'r') && (cmd[1] == 'E' || cmd[1] == 'e') &&
        (cmd[2] == 'M' || cmd[2] == 'm') && cmd[3] && !isalnum((unsigned char)cmd[3]) &&
        cmd[3] != '_') return 0;

    /* RESET: drop everything currently held. DuckyScript uses it to recover a
     * known state after a chord, and it is the safe thing to do before a
     * payload hands control back. */
    /* ---- device-level commands (no keystrokes) ---- */
    {
        uint8_t sp = DSP_NONE;
        if      (kw(cmd, "LED_OFF"))  sp = DSP_LED_OFF;
        else if (kw(cmd, "LED_R") || kw(cmd, "LED_RED"))   sp = DSP_LED_R;
        else if (kw(cmd, "LED_G") || kw(cmd, "LED_GREEN")) sp = DSP_LED_G;
        else if (kw(cmd, "SAVE_HOST_KEYBOARD_LOCK_STATE"))    sp = DSP_SAVE_LOCKS;
        else if (kw(cmd, "RESTORE_HOST_KEYBOARD_LOCK_STATE")) sp = DSP_RESTORE_LOCKS;
        else if (kw(cmd, "WAIT_FOR_BUTTON_PRESS")) sp = DSP_WAIT_BUTTON;
        else if (kw(cmd, "ENABLE_BUTTON"))  sp = DSP_BUTTON_ENABLE;
        else if (kw(cmd, "DISABLE_BUTTON")) sp = DSP_BUTTON_DISABLE;
        else if (kw(cmd, "SAVE_ATTACKMODE"))    sp = DSP_SAVE_ATTACKMODE;
        else if (kw(cmd, "RESTORE_ATTACKMODE")) sp = DSP_RESTORE_ATTACKMODE;
        /* Accepted and ignored: these manage files on a Ducky's own mass
         * storage, which this device does not present. Refusing them would
         * fail otherwise-portable payloads for no benefit. */
        else if (kw(cmd, "HIDE_PAYLOAD") || kw(cmd, "RESTORE_PAYLOAD")) sp = DSP_NOP;
        /* Storage waits: this device presents no mass-storage interface, so
         * there is no activity to wait for. Accepted so the surrounding payload
         * still runs, rather than failing on a line about a drive we do not
         * pretend to have. */
        else if (kw(cmd, "WAIT_FOR_STORAGE_ACTIVITY") ||
                 kw(cmd, "WAIT_FOR_STORAGE_INACTIVITY")) sp = DSP_NOP;
        if (sp != DSP_NONE) {
            out[0].kind = DUCKY_SPECIAL; out[0].special = sp;
            out[0].key = 0; out[0].mods = 0; out[0].text[0] = 0;
            return 1;
        }
    }
    if (kw(cmd, "ATTACKMODE")) {
        /* HID is what this device is. STORAGE would need a mass-storage
         * interface it does not have, and silently continuing would leave a
         * payload waiting for a drive that never appears - so it is refused. */
        out[0].kind = DUCKY_SPECIAL; out[0].key = 0; out[0].mods = 0;
        copy_bounded(out[0].text, sizeof(out[0].text), rest);
        if (kw(rest, "OFF"))                       out[0].special = DSP_ATTACKMODE_OFF;
        else if (strstr(rest, "STORAGE") || strstr(rest, "storage"))
            out[0].special = DSP_ATTACKMODE_STORAGE;
        else                                       out[0].special = DSP_ATTACKMODE_HID;
        return 1;
    }
    if (kw(cmd, "EXFIL")) {
        out[0].kind = DUCKY_SPECIAL; out[0].special = DSP_EXFIL;
        out[0].key = 0; out[0].mods = 0;
        copy_bounded(out[0].text, sizeof(out[0].text), rest);
        return 1;
    }
    if (kw(cmd, "INJECT_MOD")) {
        /* Press the modifiers with no key, so the host sees the modifier on its
         * own - what opens the Start menu from a lone GUI press. */
        uint8_t m = 0, one;
        char t[24]; size_t ti = 0; const char *q = rest;
        for (;; q++) {
            if (*q == ' ' || *q == 0) {
                if (ti) { t[ti] = 0; if (hid_modifier(t, &one)) m |= one; ti = 0; }
                if (*q == 0) break;
            } else if (ti < sizeof(t) - 1) t[ti++] = *q;
        }
        if (!m) {
            /* Bare INJECT_MOD: the modifier is on the following line in the
             * official payloads. Accepting it as a no-op keeps those files
             * valid instead of failing on a line that is genuinely correct. */
            out[0].kind = DUCKY_SPECIAL; out[0].special = DSP_NOP;
            out[0].key = 0; out[0].mods = 0; out[0].text[0] = 0;
            return 1;
        }
        out[0].kind = DUCKY_KEY; out[0].mods = m; out[0].key = 0; out[0].delay_ms = 0;
        return 1;
    }
    if (kw(cmd, "RESET")) {
        out[0].kind = DUCKY_RELEASE; out[0].key = 0; out[0].mods = 0;
        return 1;
    }
    /* HOLD <keys> / RELEASE: press modifiers and keep them down across the
     * following lines, until RELEASE. */
    if (kw(cmd, "HOLD")) {
        uint8_t hm = 0, hk = 0; bool got = false;
        char t2[24]; size_t q2 = 0; const char *r2 = rest;
        for (;; r2++) {
            if (*r2 == ' ' || *r2 == '-' || *r2 == 0) {
                if (q2) { t2[q2] = 0; uint8_t mm, kk;
                          if (hid_modifier(t2, &mm)) { hm |= mm; got = true; }
                          else if (hid_named_key(t2, &kk)) { hk = kk; got = true; }
                          q2 = 0; }
                if (*r2 == 0) break;
            } else if (q2 < sizeof(t2) - 1) t2[q2++] = *r2;
        }
        if (!got) return 0;
        out[0].kind = DUCKY_HOLD; out[0].mods = hm; out[0].key = hk;
        return 1;
    }
    if (kw(cmd, "RELEASE")) {
        out[0].kind = DUCKY_RELEASE; out[0].key = 0; out[0].mods = 0;
        return 1;
    }
    /* WAIT_FOR_*: block until the host reports a lock-key state. The OUT
     * endpoint carries this back from the operating system, so it doubles as a
     * signal that the OS is alive - and as a crude channel a payload can be
     * driven by. */
    if (kw(cmd, "WAIT_FOR_CAPS_ON")     || kw(cmd, "WAIT_FOR_CAPS_OFF")   ||
        kw(cmd, "WAIT_FOR_CAPS_CHANGE") || kw(cmd, "WAIT_FOR_NUM_ON")     ||
        kw(cmd, "WAIT_FOR_NUM_OFF")     || kw(cmd, "WAIT_FOR_NUM_CHANGE") ||
        kw(cmd, "WAIT_FOR_SCROLL_ON")   || kw(cmd, "WAIT_FOR_SCROLL_OFF") ||
        kw(cmd, "WAIT_FOR_SCROLL_CHANGE")) {
        uint8_t mask = HID_LED_CAPSLOCK, want = 2;
        if      (strstr(cmd, "NUM"))    mask = HID_LED_NUMLOCK;
        else if (strstr(cmd, "SCROLL")) mask = HID_LED_SCROLL;
        if      (strstr(cmd, "_ON"))    want = 1;
        else if (strstr(cmd, "_OFF"))   want = 0;
        memset(&out[0], 0, sizeof(out[0]));
        out[0].kind = DUCKY_WAIT; out[0].wait_mask = mask; out[0].wait_want = want;
        return 1;
    }
    {   /* RANDOM_* */
        int rr = emit_random(st, cmd, out, max);
        if (rr >= 0) return rr;
    }
    if (kw(cmd, "REPEAT")) {
        /* Two forms in the wild: "REPEAT 3" repeats the previous command, and
         * "REPEAT 4 TAB" repeats the command written after the count. The
         * second appears throughout the official library. */
        int r = atoi(rest);
        const char *after = rest;
        while (*after && *after != ' ') after++;
        while (*after == ' ') after++;
        if (r > 0 && *after) {
            copy_bounded(st->last_cmd, sizeof(st->last_cmd), after);
            st->repeat = r;
            return 0;
        }
        st->repeat = r > 0 ? r : 0;
        return 0;
    }
    if (kw(cmd, "DEFAULTDELAY") || kw(cmd, "DEFAULT_DELAY")) {
        st->default_delay_ms = (uint32_t)atoi(rest); return 0;
    }
    if (kw(cmd, "STRINGDELAY") || kw(cmd, "STRING_DELAY")) {
        st->string_delay_ms = (uint32_t)atoi(rest); return 0;
    }

    /* everything below is a real command; remember it for REPEAT */
    strncpy(st->last_cmd, buf, sizeof(st->last_cmd) - 1);
    st->last_cmd[sizeof(st->last_cmd) - 1] = 0;

    if (kw(cmd, "DELAY") && *rest == 0) {      /* bare DELAY = one default delay */
        out[0].kind = DUCKY_DELAY;
        out[0].delay_ms = st->default_delay_ms ? st->default_delay_ms : 100;
        out[0].key = 0; out[0].mods = 0;
        return 1;
    }
    if (kw(cmd, "DELAY")) {
        out[0].kind = DUCKY_DELAY; out[0].delay_ms = (uint32_t)atoi(rest);
        out[0].key = 0; out[0].mods = 0;
        return 1;
    }
    if (kw(cmd, "UNICODE")) {
        const char *h = rest;
        if ((h[0] == 'U' || h[0] == 'u') && h[1] == '+') h += 2;
        uint32_t cp = (uint32_t)strtoul(h, NULL, 16);
        if (!cp) return 0;
        uint8_t uk, um;
        if (layout_utf8_key(st->layout, cp, &uk, &um)) {
            memset(&out[0], 0, sizeof(out[0]));
            out[0].kind = DUCKY_KEY; out[0].key = uk; out[0].mods = um;
            return 1;
        }
        if (st->target_os == OS_MAC) {
            int adds = mac_option_seq(cp, out, max);
            if (adds) return adds;
        }
        return unicode_seq(cp, st->target_os, out, max);
    }
    if (kw(cmd, "STRING") || kw(cmd, "STRINGLN")) {
        const bool ln = kw(cmd, "STRINGLN");
        const char *end = rest;
        int k = emit_string(rest, st->layout, st->target_os,
                            st->string_delay_ms, out, max, &end);
        if (*end) {                       /* more text than actions: continue it */
            st->pending = end; st->pending_ln = ln;
            return k;
        }
        if (ln && k < max) { memset(&out[k], 0, sizeof(out[k]));
                             out[k].kind = DUCKY_KEY; out[k].key = HID_KEY_ENTER; k++; }
        return k;
    }

    if (kw(cmd, "MOUSEMOVE")) {
        int x = 0, y = 0; sscanf(rest, "%d %d", &x, &y);
        out[0].kind = DUCKY_MOUSE; out[0].mx = clamp127(x); out[0].my = clamp127(y);
        out[0].wheel = 0; out[0].buttons = 0; out[0].key = 0; out[0].mods = 0;
        return 1;
    }
    if (kw(cmd, "MOUSECLICK") || kw(cmd, "MOUSEBUTTON") ||
        (kw(cmd, "MOUSE") && kw_prefix(rest, "CLICK", 5))) {
        if (kw(cmd, "MOUSE")) { rest += 5; while (*rest == ' ') rest++; }
        /* Named (LEFT/RIGHT/MIDDLE) or numbered (1/2/4) - both appear. */
        uint8_t mb = mouse_button(rest);
        if (!mb && rest[0] >= '1' && rest[0] <= '4') mb = (uint8_t)(rest[0] - '0');
        out[0].kind = DUCKY_MOUSE; out[0].buttons = mb;
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
        /* DuckyScript writes chords both ways: "CTRL ALT DELETE" and
         * "CTRL-ALT-DELETE". A '-' only separates when it is joining tokens;
         * a lone '-' is the minus key itself, so it must still type. */
        bool sep = (*q == ' ') || (*q == '-' && p > 0 && q[1] != 0 && q[1] != ' ');
        if (sep || *q == 0) {
            if (p > 0) {
                tok[p] = 0;
                uint8_t m, k;
                if (hid_modifier(tok, &m))        mods |= m;
                else if (hid_named_key(tok, &k)) { key = k; have_key = true; }
                else if (p == 1) {
                    uint8_t am = 0;
                    if (hid_from_ascii_layout(tok[0], st->layout, &k, &am)) {
                        key = k; have_key = true;
                        /* Letters are named case-insensitively in a chord:
                         * "CTRL A" means Ctrl+A, not Ctrl+Shift+A. Symbols are
                         * the opposite - "CTRL +" needs the very shift that
                         * PRODUCES '+', and throwing it away pressed the
                         * unshifted key underneath it instead. */
                        if (!isalpha((unsigned char)tok[0])) mods |= am;
                    } else invalid = true;
                }
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
