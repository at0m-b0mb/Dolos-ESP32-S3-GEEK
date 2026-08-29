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

typedef enum { LAYOUT_US = 0, LAYOUT_UK, LAYOUT_DE, LAYOUT_FR, LAYOUT_ES, LAYOUT__COUNT } kb_layout_t;

kb_layout_t layout_from_name(const char *name);   /* "us","uk","de","fr","es" */
const char *layout_name(kb_layout_t l);

/* ASCII -> (scan code, modifier bits) for the given layout. false if unmapped. */
bool hid_from_ascii_layout(char c, kb_layout_t layout, uint8_t *key, uint8_t *mods);
#endif
