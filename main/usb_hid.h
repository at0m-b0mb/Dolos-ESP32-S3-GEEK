/*
 * usb_hid.h - USB HID keyboard device (TinyUSB) for Dolos.
 *
 * The board enumerates to the host as a standard HID keyboard. It sends NOTHING
 * on its own; the payload player calls usb_hid_tap() only after the operator has
 * armed the device by hand. usb_hid_ready() reports when the host has mounted us
 * and will accept a report.
 */
#ifndef DOLOS_USB_HID_H
#define DOLOS_USB_HID_H
#include <stdbool.h>
#include <stdint.h>

void usb_hid_init(void);
bool usb_hid_ready(void);                       /* mounted + endpoint free */
bool usb_hid_mounted(void);                     /* host has enumerated us  */
void usb_hid_tap(uint8_t mods, uint8_t key);    /* press then release one chord */
#endif
