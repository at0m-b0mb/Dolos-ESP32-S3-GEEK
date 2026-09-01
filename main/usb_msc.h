/*
 * usb_msc.h - expose ONE partition of the microSD card as a USB drive.
 *
 * This is what makes `ATTACKMODE HID STORAGE` mean something on this device.
 *
 * WHY A PARTITION AND NOT THE WHOLE CARD
 *
 * ESP-IDF ships a helper that hands the entire card to the host. That works,
 * but it also hands over the payloads, the audit log and the boot log - every
 * file the operator put on the device. On an engagement the point of storage is
 * usually the opposite: give the target machine somewhere to write, and keep
 * your own tooling out of sight.
 *
 * So the MSC callbacks are implemented here instead, and they map the host's
 * sector numbers onto a WINDOW inside the card: the chosen partition, and
 * nothing else. Sector 0 for the host is the first sector of that partition.
 * The host sees a plain FAT volume of exactly that size and cannot address a
 * byte outside it - not because it is asked not to, but because the arithmetic
 * gives it nowhere else to go.
 *
 * OWNERSHIP IS EXCLUSIVE
 *
 * Two writers on one filesystem corrupts it. While the window is exposed the
 * firmware does not touch the card at all; while it is not, the host is told
 * there is no medium. There is no state in which both sides can write.
 */
#ifndef DOLOS_USB_MSC_H
#define DOLOS_USB_MSC_H

#include <stdbool.h>
#include <stdint.h>
#include "sdmmc_cmd.h"

/* Read the partition table and pick the window to expose.
 *
 * `want` is the 1-based MBR partition number the operator asked for. Partition
 * 1 is where the firmware keeps payloads, so exposing it would defeat the
 * purpose; the default is 2. Returns false (and exposes nothing) if the card
 * has no such partition - a card with one partition simply has no share to
 * give, which is a fact to report, not a failure to work around. */
bool usb_msc_init(sdmmc_card_t *card, int want);

/* Is there a window available to expose at all? */
bool usb_msc_available(void);

/* Hand the window to the host, or take it back. Returns the new state.
 * Taking it back is what lets the firmware read payloads again. */
bool usb_msc_expose(bool on);
bool usb_msc_exposed(void);

/* For the screen: partition number, and its size in megabytes. */
int      usb_msc_partition(void);
uint32_t usb_msc_size_mb(void);

/* Has the host written to the window since it was exposed? Worth showing: it
 * is the difference between "a drive was offered" and "something was put on
 * it", which is exactly what an engagement log wants to record. */
bool usb_msc_host_wrote(void);

#endif /* DOLOS_USB_MSC_H */
