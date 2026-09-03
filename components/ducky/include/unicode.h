/*
 * unicode.h - "type anything": UTF-8 decode + per-OS Unicode injection.
 *
 * A HID keyboard can only send scan codes, so to type a character that is not on
 * the target layout we drive the target OS's own Unicode input method:
 *   Windows  Alt + numpad '+' + hex   (needs EnableHexNumpad set once)
 *   Linux    Ctrl+Shift+U + hex       (IBus/GTK, works out of the box)
 *   macOS    Option + hex             (needs the "Unicode Hex Input" source)
 * Pure C, host-testable: the sequence is emitted as ducky_actions the player
 * replays, using DUCKY_HOLD / DUCKY_KEY / DUCKY_RELEASE so a modifier can be
 * held across several keystrokes (which every one of these methods requires).
 */
#ifndef DOLOS_UNICODE_H
#define DOLOS_UNICODE_H
#include <stdint.h>
#include <stddef.h>
#include "ducky.h"   /* target_os_t + ducky_action_t */

target_os_t os_from_name(const char *name);   /* windows|win, linux, mac|macos|osx */
const char *os_name(target_os_t os);

/* Decode one UTF-8 codepoint at *p, advance *p, return bytes consumed (0 = end,
 * invalid bytes decode as U+FFFD and consume 1). */
int utf8_next(const char **p, uint32_t *cp);

/* Emit the key sequence that types codepoint cp on target os, into out (up to
 * max actions). Returns the count (0 if it does not fit / unsupported). */
/* macOS Option sequences for accented characters on the STOCK US keyboard.
 * Returns 0 if this codepoint has no such sequence. Prefer it over
 * unicode_seq() on macOS: the hex method needs a layout nobody has selected. */
int mac_option_seq(uint32_t cp, ducky_action_t *out, int max);

int unicode_seq(uint32_t cp, target_os_t os, ducky_action_t *out, int max);
#endif
