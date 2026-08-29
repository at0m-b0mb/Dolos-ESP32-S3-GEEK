#include "usb_hid.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"

static const char *TAG = "usb_hid";
static uint8_t s_half_delay = 3;    /* ms each side of a keystroke (speed) */
static volatile uint8_t s_leds = 0; /* host LED state: exfil return channel */

/* Report IDs for the composite HID device. */
enum { RID_KEYBOARD = 1, RID_MOUSE, RID_CONSUMER };

/* Keyboard + mouse + consumer-control in one HID interface. */
static const uint8_t s_hid_report[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(RID_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(RID_MOUSE)),
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(RID_CONSUMER)),
};

#define DOLOS_CFG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)
static const uint8_t s_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, DOLOS_CFG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_NONE, sizeof(s_hid_report),
                       0x81, 16, 1),
};

/* String + device descriptors are filled from config so the USB identity can be
 * set by the operator for an authorized allow-list test. Defaults are generic
 * "Dolos". */
static char s_mfr[24]  = "Dolos";
static char s_prod[32] = "Dolos HID Keyboard";
static const char *s_strdesc[5] = { (const char[]){0x09, 0x04}, s_mfr, s_prod, "000001", "Dolos HID" };

static tusb_desc_device_t s_dev = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200, .bDeviceClass = 0x00, .bDeviceSubClass = 0x00, .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A, .idProduct = 0x4004,        /* Espressif default */
    .bcdDevice = 0x0100, .iManufacturer = 1, .iProduct = 2, .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

/* --- TinyUSB HID callbacks --- */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) { (void)instance; return s_hid_report; }

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)type; (void)buffer; (void)reqlen; return 0;
}

/* The host writes the keyboard LED state here (CapsLock/NumLock/ScrollLock).
 * We keep it as a 3-bit return channel a payload/operator can read on-screen. */
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance; (void)report_id;
    if (type == HID_REPORT_TYPE_OUTPUT && bufsize >= 1) s_leds = buffer[0];
}

void usb_hid_init(uint16_t vid, uint16_t pid, const char *mfr, const char *product)
{
    if (vid) s_dev.idVendor = vid;
    if (pid) s_dev.idProduct = pid;
    if (mfr && *mfr)     { strncpy(s_mfr, mfr, sizeof(s_mfr) - 1); s_mfr[sizeof(s_mfr)-1] = 0; }
    if (product && *product) { strncpy(s_prod, product, sizeof(s_prod) - 1); s_prod[sizeof(s_prod)-1] = 0; }

    const tinyusb_config_t cfg = {
        .device_descriptor = &s_dev,
        .string_descriptor = s_strdesc,
        .string_descriptor_count = 5,
        .external_phy = false,
        .configuration_descriptor = s_config,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&cfg));
    ESP_LOGI(TAG, "USB HID (kbd+mouse+consumer) up as %04X:%04X '%s' (SAFE)",
             s_dev.idVendor, s_dev.idProduct, s_prod);
}

bool usb_hid_ready(void)   { return tud_mounted() && tud_hid_ready(); }
bool usb_hid_mounted(void) { return tud_mounted(); }
uint8_t usb_hid_leds(void) { return s_leds; }
void usb_hid_set_speed(uint8_t half_delay_ms) { s_half_delay = half_delay_ms; }

static void wait_ready(void)
{
    for (int i = 0; i < 200 && !tud_hid_ready(); i++) vTaskDelay(pdMS_TO_TICKS(2));
}

void usb_hid_tap(uint8_t mods, uint8_t key)
{
    uint8_t keys[6] = { key, 0, 0, 0, 0, 0 };
    wait_ready(); tud_hid_keyboard_report(RID_KEYBOARD, mods, key ? keys : NULL);
    vTaskDelay(pdMS_TO_TICKS(s_half_delay));
    wait_ready(); tud_hid_keyboard_report(RID_KEYBOARD, 0, NULL);
    vTaskDelay(pdMS_TO_TICKS(s_half_delay));
}

void usb_hid_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
    wait_ready();
    tud_hid_mouse_report(RID_MOUSE, buttons, dx, dy, wheel, 0);
    vTaskDelay(pdMS_TO_TICKS(s_half_delay));
    if (buttons) {                        /* a click: release the button */
        wait_ready();
        tud_hid_mouse_report(RID_MOUSE, 0, 0, 0, 0, 0);
        vTaskDelay(pdMS_TO_TICKS(s_half_delay));
    }
}

void usb_hid_consumer(uint16_t usage)
{
    wait_ready(); tud_hid_report(RID_CONSUMER, &usage, 2);
    vTaskDelay(pdMS_TO_TICKS(s_half_delay));
    uint16_t zero = 0;
    wait_ready(); tud_hid_report(RID_CONSUMER, &zero, 2);
    vTaskDelay(pdMS_TO_TICKS(s_half_delay));
}
