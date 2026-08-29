/*
 * dolos_main.c - Dolos firmware entry for the Waveshare ESP32-S3-GEEK.
 *
 * A USB-HID (BadUSB) payload runner FOR AUTHORIZED LAB / EDUCATIONAL USE ONLY.
 *
 * SAFETY MODEL (why this will not fire by accident):
 *   - It boots SAFE and enumerates as a keyboard that sends NOTHING.
 *   - Firing requires TWO deliberate holds of the BOOT button at the device:
 *       SAFE --hold--> ARMED --hold--> 3-2-1 COUNTDOWN --> RUNNING.
 *     A tap aborts at any stage; ARMED times out back to SAFE on its own.
 *   - Holding BOOT while powering on enters FLASH MODE: USB-HID is not started
 *     at all (stays a plain serial device), so it can never type and is easy to
 *     re-flash.
 *   - The payload is plain text on the SD card (/sdcard/PAYLOAD.TXT) that you
 *     can read; with no card it uses a harmless built-in demo.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"

#include "board.h"
#include "display.h"
#include "dui.h"
#include "usb_hid.h"
#include "payload.h"

static const char *TAG = "dolos";

#define HOLD_MS       1200u    /* press this long to advance a safety stage */
#define ARMED_TMO_MS  8000u    /* ARMED falls back to SAFE if you wait      */
#define COUNTDOWN_MS  3000u

static dui_mode_t   s_mode = DUI_SAFE;
static bool         s_flash_mode;
static char         s_payload_buf[4096];
static const char  *s_payload;
static const char  *s_payload_name = "demo";
static int          s_total_lines, s_cur_line;
static volatile bool s_abort;
static volatile bool s_run_done;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ---- SD (optional): mount so the payload can be read from a card ---- */
static bool sd_mount(void)
{
    spi_bus_config_t bus = { .sclk_io_num = BOARD_SD_PIN_SCLK, .mosi_io_num = BOARD_SD_PIN_MOSI,
                             .miso_io_num = BOARD_SD_PIN_MISO, .quadwp_io_num = -1,
                             .quadhd_io_num = -1, .max_transfer_sz = 4000 };
    if (spi_bus_initialize(BOARD_SD_SPI_HOST, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = BOARD_SD_SPI_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = BOARD_SD_PIN_CS; slot.host_id = BOARD_SD_SPI_HOST;
    esp_vfs_fat_sdmmc_mount_config_t mc = { .format_if_mount_failed = false, .max_files = 3 };
    sdmmc_card_t *card = NULL;
    return esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mc, &card) == ESP_OK;
}

/* ---- BOOT button: returns one event per call ---- */
typedef enum { BTN_NONE = 0, BTN_TAP, BTN_HOLD } btn_evt_t;
static btn_evt_t button_event(void)
{
    static bool down = false; static uint32_t t0 = 0; static bool hold_fired = false;
    bool pressed = gpio_get_level(BOARD_BTN_PIN) == 0;   /* active low */
    if (pressed && !down)  { down = true; t0 = now_ms(); hold_fired = false; }
    else if (pressed && down && !hold_fired && now_ms() - t0 >= HOLD_MS) {
        hold_fired = true; return BTN_HOLD;
    } else if (!pressed && down) {
        down = false;
        if (!hold_fired) return BTN_TAP;
    }
    return BTN_NONE;
}

/* ---- payload playback task ---- */
static void on_progress(int cur, int total, void *u) { (void)u; s_cur_line = cur; s_total_lines = total; }
static void payload_task(void *arg)
{
    (void)arg;
    payload_ctx_t ctx = { .abort = &s_abort, .progress = on_progress, .user = NULL };
    payload_run(s_payload, &ctx);
    s_run_done = true;
    vTaskDelete(NULL);
}

static void ui_task(void *arg)
{
    (void)arg;
    canvas_t *cv = display_canvas();
    if (cv) { dui_render_splash(cv); display_flush(); }
    vTaskDelay(pdMS_TO_TICKS(1500));

    uint32_t stage_ms = now_ms();
    dui_state_t st; memset(&st, 0, sizeof(st));
    st.payload_name = s_payload_name;

    for (;;) {
        btn_evt_t e = button_event();
        uint32_t t = now_ms();

        switch (s_mode) {
        case DUI_SAFE:
            if (e == BTN_HOLD && !s_flash_mode) { s_mode = DUI_ARMED; stage_ms = t; }
            break;
        case DUI_ARMED:
            if (e == BTN_TAP)  { s_mode = DUI_SAFE; }
            else if (e == BTN_HOLD) { s_mode = DUI_COUNTDOWN; stage_ms = t; }
            else if (t - stage_ms > ARMED_TMO_MS) { s_mode = DUI_SAFE; }
            break;
        case DUI_COUNTDOWN:
            if (e == BTN_TAP) { s_mode = DUI_SAFE; break; }
            if (t - stage_ms >= COUNTDOWN_MS) {
                s_abort = false; s_run_done = false; s_cur_line = 0;
                s_mode = DUI_RUNNING; stage_ms = t;
                xTaskCreate(payload_task, "payload", 4096, NULL, 6, NULL);
            }
            break;
        case DUI_RUNNING:
            if (e == BTN_TAP) s_abort = true;
            if (s_run_done)   { s_mode = DUI_DONE; stage_ms = t; }
            break;
        case DUI_DONE:
            if (e == BTN_TAP || t - stage_ms > 4000) s_mode = DUI_SAFE;
            break;
        }

        st.mode = s_mode;
        st.usb_mounted = s_flash_mode ? false : usb_hid_mounted();
        st.total_lines = s_total_lines ? s_total_lines : payload_count_lines(s_payload);
        st.cur_line = s_cur_line;
        st.countdown = 3 - (int)((t - stage_ms) / 1000);
        if (st.countdown < 1) st.countdown = 1;
        st.anim++;

        if (cv) { dui_render(cv, &st); display_flush(); }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase()); ESP_ERROR_CHECK(nvs_flash_init());
    }

    /* Read BOOT at power-on: held => FLASH MODE (never becomes a keyboard). */
    gpio_config_t io = { .pin_bit_mask = 1ULL << BOARD_BTN_PIN, .mode = GPIO_MODE_INPUT,
                         .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&io);
    s_flash_mode = (gpio_get_level(BOARD_BTN_PIN) == 0);

    display_init();

    if (sd_mount()) s_payload = payload_load(s_payload_buf, sizeof(s_payload_buf));
    else            s_payload = DOLOS_DEMO_PAYLOAD;
    s_payload_name = (s_payload == DOLOS_DEMO_PAYLOAD) ? "demo" : "PAYLOAD.TXT";
    s_total_lines = payload_count_lines(s_payload);

    if (s_flash_mode) {
        ESP_LOGW(TAG, "FLASH MODE (BOOT held) - USB-HID NOT started; device is safe to re-flash");
    } else {
        usb_hid_init();
    }

    ESP_LOGI(TAG, "Dolos up. mode=%s payload=%s lines=%d",
             s_flash_mode ? "FLASH/SAFE" : "HID", s_payload_name, s_total_lines);

    xTaskCreate(ui_task, "dolos_ui", 6144, NULL, 5, NULL);
}
