/*
 * layout.h - keyboard-layout profiles for international targets.
 *
 * A HID keyboard sends SCAN CODES, not characters; the target OS turns a scan
 * code into a character using ITS keyboard layout. So to type "!" on a German
 * (QWERTZ) target we must send the scan code that is "!" on that layout, which
 * differs from US-QWERTY. This maps an ASCII character to (scan code, modifiers)
 * for a chosen layout.
 *
 * US is exact. UK/DE/FR/ES cover the common letters, digits and symbols and are
 * best-effort for AltGr symbols - verify against your specific target. Anything
 * a layout does not override falls back to the US position.
 */
#ifndef DOLOS_LAYOUT_H
#define DOLOS_LAYOUT_H
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    LAYOUT_US = 0, LAYOUT_UK, LAYOUT_DE, LAYOUT_FR, LAYOUT_ES,
    LAYOUT_IT, LAYOUT_PT, LAYOUT_SE, LAYOUT_CH, LAYOUT_LATAM,
    LAYOUT__COUNT
} kb_layout_t;

/* Is this Unicode codepoint a KEY ON THIS LAYOUT?
 *
 * On a German keyboard "a-umlaut" is one keystroke, not a Unicode escape
 * sequence. Typing it through the operating system's Alt+numpad method instead
 * costs seven USB reports, needs Num Lock on, needs EnableHexNumpad set in the
 * registry, and produces nothing at all on a login screen. Looking the
 * character up on the target layout first makes it a single keypress that works
 * everywhere - the OS method stays as the fallback for characters the layout
 * genuinely cannot produce.
 *
 * Layout data follows the tables in SpacehuhnTech/WiFiDuck (MIT). */
bool layout_utf8_key(kb_layout_t layout, uint32_t cp, uint8_t *key, uint8_t *mods);

/* A DEAD-KEY sequence: on many layouts an accented vowel is not a key at all,
 * it is an accent key followed by the letter (acute then "a" gives a-acute).
 * Returns the two keystrokes to send, in order. Spanish, Portuguese and German
 * need this for their accented vowels, which is most of their written text. */
bool layout_utf8_combo(kb_layout_t layout, uint32_t cp,
                       uint8_t *dead_key, uint8_t *dead_mods,
                       uint8_t *base_key, uint8_t *base_mods);

kb_layout_t layout_from_name(const char *name);   /* us,uk,de,fr,es,it,pt,se,ch,latam */
const char *layout_name(kb_layout_t l);

/* ASCII -> (scan code, modifier bits) for the given layout. false if unmapped. */
bool hid_from_ascii_layout(char c, kb_layout_t layout, uint8_t *key, uint8_t *mods);
#endif
