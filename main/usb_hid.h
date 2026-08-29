/*
 * usb_hid.h - composite USB-HID device (keyboard + mouse + consumer control).
 *
 * Enumerates as a standard HID device and sends NOTHING on its own; the payload
 * player emits reports only after the operator arms Dolos by hand. The host's
 * keyboard-LED state is captured as a 3-bit return channel (usb_hid_leds). The
 * USB identity (VID/PID/strings) is settable for authorized allow-list testing.
 */
#ifndef DOLOS_USB_HID_H
#define DOLOS_USB_HID_H
#include <stdbool.h>
#include <stdint.h>

void usb_hid_init(uint16_t vid, uint16_t pid, const char *mfr, const char *product);
bool usb_hid_ready(void);
bool usb_hid_mounted(void);
void usb_hid_set_speed(uint8_t half_delay_ms);
void usb_hid_tap(uint8_t mods, uint8_t key);            /* one key chord      */
void usb_hid_hold(uint8_t mods);                        /* press + hold modifiers */
void usb_hid_key(uint8_t tap_mods, uint8_t held_after, uint8_t key); /* tap, keep held */
void usb_hid_release(void);                             /* release everything */
void usb_hid_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);
void usb_hid_consumer(uint16_t usage);                  /* media / consumer   */
uint8_t usb_hid_leds(void);                             /* host LED bitmap    */
#endif
