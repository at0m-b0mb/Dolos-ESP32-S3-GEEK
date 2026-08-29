#include "usb_hid.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"

static const char *TAG = "usb_hid";

/* Standard boot-keyboard report descriptor. */
static const uint8_t s_hid_report[] = { TUD_HID_REPORT_DESC_KEYBOARD() };

/* Configuration: one HID interface, IN endpoint 0x81. */
#define DOLOS_CFG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)
static const uint8_t s_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, DOLOS_CFG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(0, 4, HID_ITF_PROTOCOL_KEYBOARD, sizeof(s_hid_report),
                       0x81, 16, 10),
};

/* String descriptors. Generic keyboard identity. */
static const char *s_strdesc[] = {
    (const char[]){0x09, 0x04},   /* 0: language = English (US) */
    "Dolos",                      /* 1: manufacturer            */
    "Dolos HID Keyboard",         /* 2: product                 */
    "000001",                     /* 3: serial                  */
    "Dolos HID",                  /* 4: HID interface           */
};

/* --- TinyUSB HID callbacks (required) --- */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report;
}
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)reqlen;
    return 0;
}
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance; (void)report_id; (void)report_type; (void)buffer; (void)bufsize;
}

void usb_hid_init(void)
{
    const tinyusb_config_t cfg = {
        .device_descriptor = NULL,                 /* use default VID/PID */
        .string_descriptor = s_strdesc,
        .string_descriptor_count = sizeof(s_strdesc) / sizeof(s_strdesc[0]),
        .external_phy = false,
        .configuration_descriptor = s_config,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&cfg));
    ESP_LOGI(TAG, "USB HID keyboard installed (SAFE - will not type until armed)");
}

bool usb_hid_ready(void)
{
    return tud_mounted() && tud_hid_ready();
}

bool usb_hid_mounted(void)
{
    return tud_mounted();
}

/* Wait (bounded) until the endpoint is free, then send one report. */
static void wait_ready(void)
{
    for (int i = 0; i < 200 && !tud_hid_ready(); i++) vTaskDelay(pdMS_TO_TICKS(2));
}

void usb_hid_tap(uint8_t mods, uint8_t key)
{
    uint8_t keys[6] = { key, 0, 0, 0, 0, 0 };
    wait_ready();
    tud_hid_keyboard_report(0, mods, key ? keys : NULL);  /* press   */
    vTaskDelay(pdMS_TO_TICKS(6));
    wait_ready();
    tud_hid_keyboard_report(0, 0, NULL);                  /* release */
    vTaskDelay(pdMS_TO_TICKS(6));
}
