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

/* ---- characters that are KEYS on a layout, not Unicode escapes ----------
 *
 * Rows are { codepoint, modifiers, key }. Data follows the locale tables in
 * SpacehuhnTech/WiFiDuck (MIT licence), which are themselves derived from the
 * published national layouts. NONE of these need the operating system's
 * Unicode input method, so they type on a login screen, over RDP, and on a
 * machine where EnableHexNumpad was never set - and they cost ONE report
 * instead of seven. */
typedef struct { uint32_t cp; uint8_t mods; uint8_t key; } uni_t;
#define SH HID_MOD_LSHIFT
#define AG HID_MOD_RALT          /* AltGr */

static const uni_t U_DE[] = {
    {0x00E4,  0, HID_KEY_QUOTE}, {0x00C4, SH, HID_KEY_QUOTE},   /* a o u umlaut */
    {0x00F6,  0, HID_KEY_SEMI},  {0x00D6, SH, HID_KEY_SEMI},
    {0x00FC,  0, HID_KEY_LBRACK},{0x00DC, SH, HID_KEY_LBRACK},
    {0x00DF,  0, HID_KEY_MINUS},                                 /* sharp s     */
    {0x00A7, SH, HID_KEY_1 + 2},     {0x00B0, SH, HID_KEY_GRAVE},
    {0x00B2, AG, HID_KEY_1 + 1},     {0x00B3, AG, HID_KEY_1 + 2},
    {0x20AC, AG, HID_KEY_A + 4},                                 /* euro        */
};
static const uni_t U_FR[] = {
    {0x00E0,  0, HID_KEY_0},     {0x00E7,  0, HID_KEY_1 + 8},
    {0x00E8,  0, HID_KEY_1 + 6}, {0x00E9,  0, HID_KEY_1 + 1},
    {0x00F9,  0, HID_KEY_QUOTE},
    {0x00A3, SH, HID_KEY_RBRACK},{0x00A7, SH, HID_KEY_SLASH},
    {0x00B0, SH, HID_KEY_MINUS}, {0x00B2,  0, HID_KEY_GRAVE},
    {0x00B5, SH, HID_KEY_BSLASH},{0x20AC, AG, HID_KEY_1 + 4},
};
static const uni_t U_ES[] = {
    {0x00F1,  0, HID_KEY_SEMI},  {0x00D1, SH, HID_KEY_SEMI},     /* enye        */
    {0x00E7,  0, HID_KEY_BSLASH},{0x00C7, SH, HID_KEY_BSLASH},
    {0x00A1,  0, HID_KEY_EQUAL}, {0x00BF, SH, HID_KEY_EQUAL},
    {0x00BA,  0, HID_KEY_GRAVE}, {0x00AA, SH, HID_KEY_GRAVE},
    {0x00B7, SH, HID_KEY_1 + 2},     {0x00AC, AG, HID_KEY_1 + 5},
    {0x20AC, AG, HID_KEY_1 + 4},
};
static const uni_t U_IT[] = {
    {0x00E0,  0, HID_KEY_QUOTE}, {0x00E8,  0, HID_KEY_LBRACK},
    {0x00E9, SH, HID_KEY_LBRACK},{0x00EC,  0, HID_KEY_EQUAL},
    {0x00F2,  0, HID_KEY_SEMI},  {0x00F9,  0, HID_KEY_BSLASH},
    {0x00E7, SH, HID_KEY_SEMI},  {0x00A3, SH, HID_KEY_1 + 2},
    {0x00A7, SH, HID_KEY_BSLASH},{0x00B0, SH, HID_KEY_QUOTE},
};
static const uni_t U_PT[] = {
    {0x00E7,  0, HID_KEY_SEMI},  {0x00C7, SH, HID_KEY_SEMI},
    {0x00BA,  0, HID_KEY_QUOTE}, {0x00AA, SH, HID_KEY_QUOTE},
    {0x00AB,  0, HID_KEY_EQUAL}, {0x00BB, SH, HID_KEY_EQUAL},
    {0x00A3, AG, HID_KEY_1 + 2},     {0x00A7, AG, HID_KEY_1 + 3},
};
static const uni_t U_SE[] = {          /* Swedish/Nordic: three extra letters */
    {0x00E5,  0, HID_KEY_LBRACK}, {0x00C5, HID_MOD_LSHIFT, HID_KEY_LBRACK},
    {0x00E4,  0, HID_KEY_QUOTE},  {0x00C4, HID_MOD_LSHIFT, HID_KEY_QUOTE},
    {0x00F6,  0, HID_KEY_SEMI},   {0x00D6, HID_MOD_LSHIFT, HID_KEY_SEMI},
    {0x00A7,  0, HID_KEY_GRAVE},  {0x20AC, HID_MOD_RALT,   HID_KEY_A + 4},
};
static const uni_t U_UK[] = {
    {0x00A3, HID_MOD_LSHIFT, HID_KEY_1 + 2},   /* pound */
    {0x20AC, HID_MOD_RALT,   HID_KEY_1 + 3},   /* euro  */
};
static const uni_t U_CH[] = {
    {0x00E4,  0, HID_KEY_QUOTE}, {0x00F6,  0, HID_KEY_SEMI},
    {0x00FC,  0, HID_KEY_LBRACK},{0x00E0, SH, HID_KEY_QUOTE},
    {0x00E8, SH, HID_KEY_LBRACK},{0x00E9, SH, HID_KEY_SEMI},
    {0x00E7, SH, HID_KEY_1 + 3}, {0x00A7,  0, HID_KEY_GRAVE},
    {0x00B0, SH, HID_KEY_GRAVE}, {0x00A3, SH, HID_KEY_BSLASH},
};
#undef SH
#undef AG

static const struct { const uni_t *t; int n; } UNI[LAYOUT__COUNT] = {
    { 0, 0 },                                        /* US: nothing extra   */
    { U_UK, (int)(sizeof(U_UK)/sizeof(U_UK[0])) },
    { U_DE, (int)(sizeof(U_DE)/sizeof(U_DE[0])) },
    { U_FR, (int)(sizeof(U_FR)/sizeof(U_FR[0])) },
    { U_ES, (int)(sizeof(U_ES)/sizeof(U_ES[0])) },
    { U_IT, (int)(sizeof(U_IT)/sizeof(U_IT[0])) },
    { U_PT, (int)(sizeof(U_PT)/sizeof(U_PT[0])) },
    { U_SE, (int)(sizeof(U_SE)/sizeof(U_SE[0])) },
    { U_CH, (int)(sizeof(U_CH)/sizeof(U_CH[0])) },
    { U_ES, (int)(sizeof(U_ES)/sizeof(U_ES[0])) },   /* LatAm ~ ES          */
};

/* ---- dead-key sequences: accent key, then the letter ---- */
typedef struct { uint32_t cp; uint8_t dmods, dkey, bmods, bkey; } combo_t;
#define SH HID_MOD_LSHIFT

/* Spanish: the acute accent lives on the apostrophe key, the grave on [. */
static const combo_t C_ES[] = {
    {0x00E1, 0, HID_KEY_QUOTE, 0,  HID_KEY_A},      {0x00C1, 0, HID_KEY_QUOTE, SH, HID_KEY_A},
    {0x00E9, 0, HID_KEY_QUOTE, 0,  HID_KEY_A + 4},  {0x00C9, 0, HID_KEY_QUOTE, SH, HID_KEY_A + 4},
    {0x00ED, 0, HID_KEY_QUOTE, 0,  HID_KEY_A + 8},  {0x00CD, 0, HID_KEY_QUOTE, SH, HID_KEY_A + 8},
    {0x00F3, 0, HID_KEY_QUOTE, 0,  HID_KEY_A + 14}, {0x00D3, 0, HID_KEY_QUOTE, SH, HID_KEY_A + 14},
    {0x00FA, 0, HID_KEY_QUOTE, 0,  HID_KEY_A + 20}, {0x00DA, 0, HID_KEY_QUOTE, SH, HID_KEY_A + 20},
    {0x00E0, 0, HID_KEY_LBRACK, 0, HID_KEY_A},      {0x00E8, 0, HID_KEY_LBRACK, 0, HID_KEY_A + 4},
    {0x00FC, SH, HID_KEY_QUOTE, 0, HID_KEY_A + 20},
};
/* German: the acute accent is the key right of zero. */
static const combo_t C_DE[] = {
    {0x00E1, 0, HID_KEY_EQUAL, 0,  HID_KEY_A},      {0x00C1, 0, HID_KEY_EQUAL, SH, HID_KEY_A},
    {0x00E9, 0, HID_KEY_EQUAL, 0,  HID_KEY_A + 4},  {0x00C9, 0, HID_KEY_EQUAL, SH, HID_KEY_A + 4},
    {0x00ED, 0, HID_KEY_EQUAL, 0,  HID_KEY_A + 8},  {0x00F3, 0, HID_KEY_EQUAL, 0, HID_KEY_A + 14},
    {0x00FA, 0, HID_KEY_EQUAL, 0,  HID_KEY_A + 20},
    {0x00E8, SH, HID_KEY_EQUAL, 0, HID_KEY_A + 4},  {0x00E0, SH, HID_KEY_EQUAL, 0, HID_KEY_A},
};
/* Portuguese: acute on the key right of P, tilde/circumflex on the next one. */
static const combo_t C_PT[] = {
    {0x00E1, 0, HID_KEY_RBRACK, 0,  HID_KEY_A},      {0x00C1, 0, HID_KEY_RBRACK, SH, HID_KEY_A},
    {0x00E9, 0, HID_KEY_RBRACK, 0,  HID_KEY_A + 4},  {0x00C9, 0, HID_KEY_RBRACK, SH, HID_KEY_A + 4},
    {0x00ED, 0, HID_KEY_RBRACK, 0,  HID_KEY_A + 8},  {0x00F3, 0, HID_KEY_RBRACK, 0,  HID_KEY_A + 14},
    {0x00FA, 0, HID_KEY_RBRACK, 0,  HID_KEY_A + 20},
    {0x00E3, 0, HID_KEY_BSLASH, 0,  HID_KEY_A},      {0x00F5, 0, HID_KEY_BSLASH, 0,  HID_KEY_A + 14},
    {0x00E2, SH, HID_KEY_BSLASH, 0, HID_KEY_A},      {0x00EA, SH, HID_KEY_BSLASH, 0, HID_KEY_A + 4},
};
#undef SH

static const struct { const combo_t *t; int n; } COMBO[LAYOUT__COUNT] = {
    { 0, 0 }, { 0, 0 },
    { C_DE, (int)(sizeof(C_DE)/sizeof(C_DE[0])) },
    { 0, 0 },
    { C_ES, (int)(sizeof(C_ES)/sizeof(C_ES[0])) },
    { 0, 0 },
    { C_PT, (int)(sizeof(C_PT)/sizeof(C_PT[0])) },
    { 0, 0 }, { 0, 0 },
    { C_ES, (int)(sizeof(C_ES)/sizeof(C_ES[0])) },   /* LatAm ~ ES */
};

bool layout_utf8_combo(kb_layout_t layout, uint32_t cp,
                       uint8_t *dead_key, uint8_t *dead_mods,
                       uint8_t *base_key, uint8_t *base_mods)
{
    if (layout >= LAYOUT__COUNT || !COMBO[layout].t) return false;
    for (int i = 0; i < COMBO[layout].n; i++) {
        if (COMBO[layout].t[i].cp != cp) continue;
        if (dead_key)  *dead_key  = COMBO[layout].t[i].dkey;
        if (dead_mods) *dead_mods = COMBO[layout].t[i].dmods;
        if (base_key)  *base_key  = COMBO[layout].t[i].bkey;
        if (base_mods) *base_mods = COMBO[layout].t[i].bmods;
        return true;
    }
    return false;
}

bool layout_utf8_key(kb_layout_t layout, uint32_t cp, uint8_t *key, uint8_t *mods)
{
    if (layout >= LAYOUT__COUNT || !UNI[layout].t) return false;
    for (int i = 0; i < UNI[layout].n; i++) {
        if (UNI[layout].t[i].cp != cp) continue;
        if (key)  *key  = UNI[layout].t[i].key;
        if (mods) *mods = UNI[layout].t[i].mods;
        return true;
    }
    return false;
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
