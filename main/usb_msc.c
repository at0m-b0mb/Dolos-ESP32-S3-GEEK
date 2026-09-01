#include "usb_msc.h"
#include <string.h>
#include "esp_log.h"
#include "tusb.h"

static const char *TAG = "usb_msc";

static sdmmc_card_t *s_card;
static uint32_t s_lba_start;      /* first sector of the exposed partition   */
static uint32_t s_lba_count;      /* its length, in sectors                  */
static int      s_part;           /* which partition (1-based), 0 = none     */
static bool     s_exposed;        /* is the host allowed at it right now?    */
static bool     s_written;        /* has the host actually written anything? */

#define SECTOR_SZ 512

bool usb_msc_available(void) { return s_card != NULL && s_lba_count > 0; }
bool usb_msc_exposed(void)   { return s_exposed; }
int  usb_msc_partition(void) { return s_part; }
bool usb_msc_host_wrote(void){ return s_written; }
uint32_t usb_msc_size_mb(void)
{
    return (uint32_t)(((uint64_t)s_lba_count * SECTOR_SZ) / (1024 * 1024));
}

bool usb_msc_init(sdmmc_card_t *card, int want)
{
    s_card = card; s_part = 0; s_lba_start = s_lba_count = 0;
    if (!card) return false;
    if (want < 1 || want > 4) want = 2;

    /* Read the MBR. The partition table is four 16-byte entries at offset
     * 0x1BE; bytes 8..11 of each are the first LBA and 12..15 the sector
     * count, both little-endian. */
    static uint8_t mbr[SECTOR_SZ];
    if (sdmmc_read_sectors(card, mbr, 0, 1) != ESP_OK) {
        ESP_LOGW(TAG, "could not read the partition table");
        return false;
    }
    if (mbr[510] != 0x55 || mbr[511] != 0xAA) {
        ESP_LOGW(TAG, "card has no partition table - it is one big volume, "
                      "so there is no second partition to share");
        return false;
    }

    const uint8_t *e = &mbr[0x1BE + (want - 1) * 16];
    uint8_t type = e[4];
    uint32_t start = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                     ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
    uint32_t count = (uint32_t)e[12] | ((uint32_t)e[13] << 8) |
                     ((uint32_t)e[14] << 16) | ((uint32_t)e[15] << 24);

    if (type == 0 || count == 0) {
        ESP_LOGW(TAG, "partition %d is empty - format the card with a second "
                      "partition to use ATTACKMODE STORAGE", want);
        return false;
    }
    /* Refuse a window that runs past the end of the card. A partition table can
     * be wrong, and clamping silently would let the host address sectors that
     * are not there. */
    if ((uint64_t)start + count > card->csd.capacity) {
        ESP_LOGE(TAG, "partition %d claims sectors past the end of the card - refusing", want);
        return false;
    }

    s_part = want; s_lba_start = start; s_lba_count = count;
    ESP_LOGI(TAG, "partition %d available to share: %lu MB (LBA %lu..%lu), type 0x%02X",
             want, (unsigned long)usb_msc_size_mb(), (unsigned long)start,
             (unsigned long)(start + count - 1), type);
    return true;
}

bool usb_msc_expose(bool on)
{
    if (on && !usb_msc_available()) return false;
    if (on != s_exposed) {
        s_exposed = on;
        if (on) s_written = false;
        ESP_LOGW(TAG, "storage %s", on ? "EXPOSED to the host" : "taken back from the host");
    }
    return s_exposed;
}

/* ---------------------------------------------------------------- TinyUSB
 *
 * The host addresses sector 0..s_lba_count-1; everything below adds
 * s_lba_start. There is no path by which a host LBA becomes a card LBA outside
 * the window, which is the whole point of doing this here rather than handing
 * over the card.
 */

void tud_msc_inquiry_cb(uint8_t lun, uint8_t vendor_id[8],
                        uint8_t product_id[16], uint8_t product_rev[4])
{
    (void)lun;
    memcpy(vendor_id,   "Dolos   ", 8);
    memcpy(product_id,  "Storage         ", 16);
    memcpy(product_rev, "1.0 ", 4);
}

/* "Is there a disk in the drive?" - no, unless a payload asked for one. */
bool tud_msc_test_unit_ready_cb(uint8_t lun)
{
    (void)lun;
    if (!s_exposed) {
        tud_msc_set_sense(lun, SCSI_SENSE_NOT_READY, 0x3A, 0x00);  /* medium not present */
        return false;
    }
    return true;
}

void tud_msc_capacity_cb(uint8_t lun, uint32_t *block_count, uint16_t *block_size)
{
    (void)lun;
    *block_count = s_exposed ? s_lba_count : 0;
    *block_size  = SECTOR_SZ;
}

bool tud_msc_start_stop_cb(uint8_t lun, uint8_t power_condition, bool start, bool load_eject)
{
    (void)lun; (void)power_condition; (void)start;
    /* The host ejecting the drive is a clean handover back to us. */
    if (load_eject && !start) usb_msc_expose(false);
    return true;
}

int32_t tud_msc_read10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                          void *buffer, uint32_t bufsize)
{
    (void)lun;
    if (!s_exposed || !s_card) return -1;
    if (lba >= s_lba_count) return -1;                      /* outside the window */
    if (offset != 0 || (bufsize % SECTOR_SZ) != 0) return -1;

    uint32_t n = bufsize / SECTOR_SZ;
    if (lba + n > s_lba_count) n = s_lba_count - lba;       /* never read past it */
    if (sdmmc_read_sectors(s_card, buffer, s_lba_start + lba, n) != ESP_OK) return -1;
    return (int32_t)(n * SECTOR_SZ);
}

int32_t tud_msc_write10_cb(uint8_t lun, uint32_t lba, uint32_t offset,
                           uint8_t *buffer, uint32_t bufsize)
{
    (void)lun;
    if (!s_exposed || !s_card) return -1;
    if (lba >= s_lba_count) return -1;
    if (offset != 0 || (bufsize % SECTOR_SZ) != 0) return -1;

    uint32_t n = bufsize / SECTOR_SZ;
    if (lba + n > s_lba_count) n = s_lba_count - lba;
    if (sdmmc_write_sectors(s_card, buffer, s_lba_start + lba, n) != ESP_OK) return -1;
    s_written = true;
    return (int32_t)(n * SECTOR_SZ);
}

int32_t tud_msc_scsi_cb(uint8_t lun, uint8_t const scsi_cmd[16],
                        void *buffer, uint16_t bufsize)
{
    (void)lun; (void)buffer; (void)bufsize;
    switch (scsi_cmd[0]) {
        case SCSI_CMD_PREVENT_ALLOW_MEDIUM_REMOVAL:
            return 0;                       /* nothing to lock; accept quietly */
        default:
            tud_msc_set_sense(lun, SCSI_SENSE_ILLEGAL_REQUEST, 0x20, 0x00);
            return -1;
    }
}

bool tud_msc_is_writable_cb(uint8_t lun)
{
    (void)lun;
    return s_exposed;
}
