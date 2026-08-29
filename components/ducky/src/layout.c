#include "layout.h"
#include "hid_keys.h"
#include "ducky.h"     /* hid_from_ascii (US base) */
#include <string.h>
#include <ctype.h>

/* One override: this character types as (key,mods) on this layout. */
typedef struct { char c; uint8_t key; uint8_t mods; } ovr_t;

#define SH HID_MOD_LSHIFT
#define AG HID_MOD_RALT

/* US letter scan codes used for swaps (A..Z = 0x04..0x1D). */
#define K(ch) (uint8_t)(HID_KEY_A + ((ch) - 'a'))

/* ---- UK (ISO): a few symbols move vs US ---- */
static const ovr_t UK[] = {
    {'"', 0x1F, SH}, {'@', HID_KEY_QUOTE, SH},           /* " on 2, @ on ' */
    {'#', HID_KEY_NONUS_HASH, 0}, {'~', HID_KEY_NONUS_HASH, SH},
    {'\\', HID_KEY_NONUS_BSLASH, 0}, {'|', HID_KEY_NONUS_BSLASH, SH},
};
/* ---- DE (QWERTZ): y<->z swap, common symbols, some via AltGr ---- */
static const ovr_t DE[] = {
    {'y', 0x1D, 0}, {'Y', 0x1D, SH}, {'z', 0x1C, 0}, {'Z', 0x1C, SH},
    {'-', HID_KEY_SLASH, 0}, {'_', HID_KEY_SLASH, SH},        /* '-' left of R-Shift */
    {'/', HID_KEY_1 + 6, SH}, {'(', HID_KEY_1 + 7, SH}, {')', HID_KEY_1 + 8, SH},
    {'=', HID_KEY_0, SH}, {'?', HID_KEY_MINUS, SH},
    {'@', K('q'), AG}, {'\\', HID_KEY_MINUS, AG}, {'{', HID_KEY_1 + 6, AG},
    {'[', HID_KEY_1 + 7, AG}, {']', HID_KEY_1 + 8, AG}, {'}', HID_KEY_0, AG},
    {'+', HID_KEY_RBRACK, 0}, {'*', HID_KEY_RBRACK, SH},
};
/* ---- FR (AZERTY): letter swaps + digit row needs shift ---- */
static const ovr_t FR[] = {
    {'a', K('q'), 0}, {'A', K('q'), SH}, {'q', K('a'), 0}, {'Q', K('a'), SH},
    {'z', K('w'), 0}, {'Z', K('w'), SH}, {'w', K('z'), 0}, {'W', K('z'), SH},
    {'m', HID_KEY_SEMI, 0}, {'M', HID_KEY_SEMI, SH},
    {'1', HID_KEY_1, SH}, {'2', HID_KEY_1+1, SH}, {'3', HID_KEY_1+2, SH},
    {'4', HID_KEY_1+3, SH}, {'5', HID_KEY_1+4, SH}, {'6', HID_KEY_1+5, SH},
    {'7', HID_KEY_1+6, SH}, {'8', HID_KEY_1+7, SH}, {'9', HID_KEY_1+8, SH}, {'0', HID_KEY_0, SH},
};
/* ---- ES: a couple of symbol positions differ ---- */
static const ovr_t ES[] = {
    {'-', HID_KEY_SLASH, 0}, {'_', HID_KEY_SLASH, SH},
    {'.', HID_KEY_DOT, 0}, {';', HID_KEY_COMMA, SH}, {':', HID_KEY_DOT, SH},
};

/* ---- CH (Swiss, QWERTZ): y<->z swap like DE ---- */
static const ovr_t CH[] = {
    {'y', 0x1D, 0}, {'Y', 0x1D, SH}, {'z', 0x1C, 0}, {'Z', 0x1C, SH},
};
/* IT/PT/SE/LATAM are QWERTY: ASCII letters + digits match US; their accented
 * characters are produced by the Unicode "type anything" path, so an empty
 * override table (US base) types those languages' text correctly. */
static const ovr_t NONE_TBL[] = { {0, 0, 0} };

static const struct { const ovr_t *t; int n; } TABLES[LAYOUT__COUNT] = {
    { 0, 0 },
    { UK, (int)(sizeof(UK)/sizeof(UK[0])) },
    { DE, (int)(sizeof(DE)/sizeof(DE[0])) },
    { FR, (int)(sizeof(FR)/sizeof(FR[0])) },
    { ES, (int)(sizeof(ES)/sizeof(ES[0])) },
    { NONE_TBL, 0 },   /* IT    */
    { NONE_TBL, 0 },   /* PT    */
    { NONE_TBL, 0 },   /* SE    */
    { CH, (int)(sizeof(CH)/sizeof(CH[0])) },  /* CH */
    { NONE_TBL, 0 },   /* LATAM */
};

kb_layout_t layout_from_name(const char *name)
{
    if (!name) return LAYOUT_US;
    char b[4] = {0};
    for (int i = 0; i < 3 && name[i]; i++) b[i] = (char)tolower((unsigned char)name[i]);
    if (!strcmp(b, "uk") || !strcmp(b, "gb")) return LAYOUT_UK;
    if (!strcmp(b, "de")) return LAYOUT_DE;
    if (!strcmp(b, "fr")) return LAYOUT_FR;
    if (!strcmp(b, "es")) return LAYOUT_ES;
    if (!strcmp(b, "it")) return LAYOUT_IT;
    if (!strcmp(b, "pt")) return LAYOUT_PT;
    if (!strcmp(b, "se") || !strcmp(b, "no") || !strcmp(b, "dk") || !strcmp(b, "fi")) return LAYOUT_SE;
    if (!strcmp(b, "ch")) return LAYOUT_CH;
    if (!strcmp(b, "la") || !strcmp(b, "lat")) return LAYOUT_LATAM;
    return LAYOUT_US;
}
const char *layout_name(kb_layout_t l)
{
    switch (l) {
        case LAYOUT_UK: return "UK"; case LAYOUT_DE: return "DE"; case LAYOUT_FR: return "FR";
        case LAYOUT_ES: return "ES"; case LAYOUT_IT: return "IT"; case LAYOUT_PT: return "PT";
        case LAYOUT_SE: return "SE"; case LAYOUT_CH: return "CH"; case LAYOUT_LATAM: return "LA";
        default: return "US";
    }
}

/* Active-layout override cache.
 *
 * The override tables are small but were scanned linearly for EVERY character
 * of every STRING (AZERTY has 20 entries, so ~10 comparisons per char on top of
 * the US lookup). Since a payload never changes layout mid-run, the table is
 * expanded once into a direct index and reused. Overrides carry real modifier
 * bytes (AltGr, not just Shift), so key and mods are cached separately rather
 * than packed.
 */
static kb_layout_t s_cached = LAYOUT__COUNT;   /* LAYOUT__COUNT = not built yet */
static uint8_t s_ovr_key[128], s_ovr_mods[128], s_ovr_has[128];

static void layout_build_cache(kb_layout_t l)
{
    memset(s_ovr_has, 0, sizeof(s_ovr_has));
    const ovr_t *t = TABLES[l].t;
    for (int i = 0; i < TABLES[l].n; i++) {
        uint8_t idx = (uint8_t)t[i].c;
        if (idx < 128) { s_ovr_key[idx] = t[i].key; s_ovr_mods[idx] = t[i].mods; s_ovr_has[idx] = 1; }
    }
    s_cached = l;
}

bool hid_from_ascii_layout(char c, kb_layout_t layout, uint8_t *key, uint8_t *mods)
{
    uint8_t idx = (uint8_t)c;
    if (idx < 128 && layout > LAYOUT_US && layout < LAYOUT__COUNT) {
        if (layout != s_cached) layout_build_cache(layout);
        if (s_ovr_has[idx]) {
            if (key)  *key  = s_ovr_key[idx];
            if (mods) *mods = s_ovr_mods[idx];
            return true;
        }
    }
    return hid_from_ascii(c, key, mods);   /* US base */
}
