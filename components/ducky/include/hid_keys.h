/*
 * hid_keys.h - USB HID Usage IDs (keyboard/keypad page 0x07) and modifier bits.
 * Values are from the USB HID Usage Tables; only the keys a payload needs.
 */
#ifndef DOLOS_HID_KEYS_H
#define DOLOS_HID_KEYS_H

/* Modifier byte (report byte 0) */
#define HID_MOD_LCTRL   0x01
#define HID_MOD_LSHIFT  0x02
#define HID_MOD_LALT    0x04
#define HID_MOD_LGUI    0x08
#define HID_MOD_RALT    0x40   /* AltGr - international symbols */

/* Letters/numbers */
#define HID_KEY_NONE    0x00
#define HID_KEY_A       0x04   /* A..Z are 0x04..0x1D */
#define HID_KEY_1       0x1E   /* 1..9 are 0x1E..0x26 */
#define HID_KEY_0       0x27
#define HID_KEY_ENTER   0x28
#define HID_KEY_ESC     0x29
#define HID_KEY_BSPACE  0x2A
#define HID_KEY_TAB     0x2B
#define HID_KEY_SPACE   0x2C
#define HID_KEY_MINUS   0x2D
#define HID_KEY_EQUAL   0x2E
#define HID_KEY_LBRACK  0x2F
#define HID_KEY_RBRACK  0x30
#define HID_KEY_BSLASH  0x31
#define HID_KEY_SEMI    0x33
#define HID_KEY_QUOTE   0x34
#define HID_KEY_GRAVE   0x35
#define HID_KEY_COMMA   0x36
#define HID_KEY_DOT     0x37
#define HID_KEY_SLASH   0x38
#define HID_KEY_CAPS    0x39
#define HID_KEY_F1      0x3A   /* F1..F12 are 0x3A..0x45 */
#define HID_KEY_PRTSCR  0x46
#define HID_KEY_INSERT  0x49
#define HID_KEY_HOME    0x4A
#define HID_KEY_PGUP    0x4B
#define HID_KEY_DELETE  0x4C
#define HID_KEY_END     0x4D
#define HID_KEY_PGDN    0x4E
#define HID_KEY_RIGHT   0x4F
#define HID_KEY_LEFT    0x50
#define HID_KEY_DOWN    0x51
#define HID_KEY_UP      0x52
#define HID_KEY_MENU    0x65
#define HID_KEY_NONUS_HASH   0x32  /* ISO key next to Enter   */
#define HID_KEY_NONUS_BSLASH 0x64  /* ISO key next to L-Shift */
#define HID_KEY_KP_PLUS      0x57
#define HID_KEY_KP1          0x59  /* KP1..KP9 = 0x59..0x61 */
#define HID_KEY_KP0          0x62

#endif /* DOLOS_HID_KEYS_H */
