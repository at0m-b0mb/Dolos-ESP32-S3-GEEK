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

/* What kind of machine are we plugged into? Worked out from how the host
 * behaved during enumeration - see usb_hid.c for the signals and their worth.
 * USB_HOST_UNKNOWN means "not enough evidence yet", never "probably Windows". */
typedef enum { USB_HOST_UNKNOWN = 0, USB_HOST_WINDOWS, USB_HOST_LINUX, USB_HOST_MAC } usb_host_os_t;
usb_host_os_t usb_hid_detect_os(void);
const char   *usb_hid_detect_why(void);   /* the evidence, in one short line */
bool          usb_hid_saw_led_report(void);
uint32_t      usb_hid_desc_requests(void);
/* Block until the host has enumerated us (or the timeout expires). Typing into
 * a host that has not finished enumerating is simply discarded. */
bool usb_hid_wait_mounted(uint32_t timeout_ms);

/* Prove the HOST is processing keystrokes, not merely that USB enumerated.
 *
 * A keyboard has an IN endpoint for keystrokes and an OUT endpoint that carries
 * lock-key LED state back from the operating system. Tapping a lock key and
 * seeing the LED state echo back is therefore an acknowledgement from the OS
 * itself - it proves the input stack is alive, which tud_mounted() does not:
 * enumeration finishes long before a login screen or a slow driver is ready to
 * accept typing, and that gap is where a payload's first characters vanish.
 * (This is the mechanism Hak5's WAIT_FOR_CAPS_CHANGE uses.)
 *
 * Returns true if the echo came back. Some hosts do not report synchronously;
 * that is not an error, so callers fall back to a fixed delay. Caps Lock is
 * always restored to the state it was found in. */
bool usb_hid_wait_host_ready(uint32_t timeout_ms);
void usb_hid_set_speed(uint8_t half_delay_ms);
void usb_hid_tap(uint8_t mods, uint8_t key);            /* one key chord      */
void usb_hid_hold(uint8_t mods);                        /* press + hold modifiers */
void usb_hid_hold_key(uint8_t mods, uint8_t key);       /* ...and a normal key too */
void usb_hid_key(uint8_t tap_mods, uint8_t held_after, uint8_t key); /* tap, keep held */
void usb_hid_release(void);                             /* release everything */
void usb_hid_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel);
void usb_hid_consumer(uint16_t usage);                  /* media / consumer   */
uint8_t usb_hid_leds(void);
/* Keystrokes the host never accepted. Non-zero means characters were lost. */
uint32_t usb_hid_drops(void);
/* Retries the LAST report needed before the host accepted it (0 = first try),
 * and whether it was accepted at all. For the injection log. */
uint16_t usb_hid_last_retries(void);
bool     usb_hid_last_ok(void);                             /* host LED bitmap    */
#endif
