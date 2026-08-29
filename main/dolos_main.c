/*
 * dolos_main.c - Dolos firmware entry for the Waveshare ESP32-S3-GEEK.
 *
 * A USB-HID (BadUSB) payload runner FOR AUTHORIZED LAB / EDUCATIONAL USE ONLY.
 *
 * SAFETY: boots SAFE, sends nothing. Firing needs two deliberate BOOT holds
 * (SAFE -> [PIN] -> ARMED -> 3-2-1 -> RUN); a tap aborts; ARMED times out.
 * Hold BOOT at power-on = FLASH MODE (USB-HID never starts). Optional arm-PIN,
 * dry-run preview, and an SD audit log make it fit a professional engagement.
 *
 * Settings come from /sdcard/DOLOS.CFG; payloads are *.txt on the SD card,
 * chosen with the on-screen picker.
 */
#include <string.h>
#include <dirent.h>
#include <stdio.h>
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
#include "dconfig.h"
#include "layout.h"

static const char *TAG = "dolos";

#define HOLD_MS       1200u
#define ARMED_TMO_MS  8000u
#define PIN_TMO_MS    15000u
#define COUNTDOWN_MS  3000u
#define MAX_PAYLOADS  16

static dui_mode_t   s_mode = DUI_SAFE;
static bool         s_flash_mode, s_sd_ok;
static dolos_config_t s_cfg;

static char  s_names[MAX_PAYLOADS][64];
static int   s_npayloads, s_sel;
static char  s_payload_buf[6144];
static const char *s_payload;
static const char *s_payload_name = "demo";
static int   s_total_lines, s_cur_line, s_last_lines;
static volatile bool s_abort, s_run_done;
static uint32_t s_run_count;

/* PIN entry scratch */
static char s_pin_buf[9];
static int  s_pin_pos, s_pin_cur;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* ---- SD ---- */
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
    esp_vfs_fat_sdmmc_mount_config_t mc = { .format_if_mount_failed = false, .max_files = 4 };
    sdmmc_card_t *card = NULL;
    return esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mc, &card) == ESP_OK;
}

static bool ends_txt(const char *n)
{
    size_t l = strlen(n);
    return l > 4 && (strcasecmp(n + l - 4, ".txt") == 0 || strcasecmp(n + l - 3, ".dd") == 0);
}

static void scan_payloads(void)
{
    s_npayloads = 0;
    DIR *d = opendir("/sdcard");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) && s_npayloads < MAX_PAYLOADS) {
        if (e->d_name[0] == '.') continue;
        if (!ends_txt(e->d_name)) continue;
        strncpy(s_names[s_npayloads], e->d_name, sizeof(s_names[0]) - 1);
        s_names[s_npayloads][sizeof(s_names[0]) - 1] = 0;
        s_npayloads++;
    }
    closedir(d);
    ESP_LOGI(TAG, "found %d payload(s) on SD", s_npayloads);
}

static void load_selected(void)
{
    if (s_npayloads > 0) {
        char path[96];
        snprintf(path, sizeof(path), "/sdcard/%s", s_names[s_sel]);
        FILE *fp = fopen(path, "r");
        if (fp) {
            int n = (int)fread(s_payload_buf, 1, sizeof(s_payload_buf) - 1, fp);
            fclose(fp);
            if (n > 0) { s_payload_buf[n] = 0; s_payload = s_payload_buf;
                         s_payload_name = s_names[s_sel]; }
        }
    } else {
        s_payload = DOLOS_DEMO_PAYLOAD;
        s_payload_name = "demo";
    }
    s_total_lines = payload_count_lines(s_payload);
}

/* ---- audit log ---- */
static void audit_write(bool aborted)
{
    if (!s_sd_ok) return;
    FILE *fp = fopen("/sdcard/DOLOS_AUDIT.LOG", "a");
    if (!fp) return;
    fprintf(fp, "run=%lu up_ms=%lu payload=%s lines=%d result=%s dry=%d layout=%s speed=%s\n",
            (unsigned long)s_run_count, (unsigned long)now_ms(), s_payload_name, s_last_lines,
            aborted ? "aborted" : "sent", s_cfg.dry_run ? 1 : 0,
            layout_name(s_cfg.layout), speed_name(s_cfg.speed));
    fclose(fp);
}

/* ---- button ---- */
typedef enum { BTN_NONE = 0, BTN_TAP, BTN_HOLD } btn_evt_t;
static btn_evt_t button_event(void)
{
    static bool down = false; static uint32_t t0 = 0; static bool hold_fired = false;
    bool pressed = gpio_get_level(BOARD_BTN_PIN) == 0;
    if (pressed && !down)  { down = true; t0 = now_ms(); hold_fired = false; }
    else if (pressed && down && !hold_fired && now_ms() - t0 >= HOLD_MS) { hold_fired = true; return BTN_HOLD; }
    else if (!pressed && down) { down = false; if (!hold_fired) return BTN_TAP; }
    return BTN_NONE;
}

/* ---- playback task ---- */
static void on_progress(int cur, int total, void *u) { (void)u; s_cur_line = cur; s_total_lines = total; }
static void payload_task(void *arg)
{
    (void)arg;
    payload_ctx_t ctx = { .abort = &s_abort, .progress = on_progress, .user = NULL,
                          .layout = s_cfg.layout, .dry_run = s_cfg.dry_run,
                          .default_delay = s_cfg.default_delay_ms };
    s_last_lines = payload_run(s_payload, &ctx);
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

    for (;;) {
        btn_evt_t e = button_event();
        uint32_t t = now_ms();

        switch (s_mode) {
        case DUI_SAFE:
            if (e == BTN_TAP && s_npayloads > 1) { s_sel = (s_sel + 1) % s_npayloads; load_selected(); }
            else if (e == BTN_HOLD && !s_flash_mode) {
                if (s_cfg.arm_pin[0]) { s_mode = DUI_PINENTRY; s_pin_pos = 0; s_pin_cur = 1; stage_ms = t; }
                else                  { s_mode = DUI_ARMED; stage_ms = t; }
            }
            break;
        case DUI_PINENTRY:
            if (e == BTN_TAP) { s_pin_cur = (s_pin_cur % 9) + 1; }
            else if (e == BTN_HOLD) {
                if (s_pin_pos < (int)sizeof(s_pin_buf) - 1) s_pin_buf[s_pin_pos++] = (char)('0' + s_pin_cur);
                s_pin_cur = 1;
                if (s_pin_pos >= (int)strlen(s_cfg.arm_pin)) {
                    s_pin_buf[s_pin_pos] = 0;
                    if (strcmp(s_pin_buf, s_cfg.arm_pin) == 0) { s_mode = DUI_ARMED; }
                    else { ESP_LOGW(TAG, "wrong PIN"); s_mode = DUI_SAFE; }
                    stage_ms = t;
                }
            }
            else if (t - stage_ms > PIN_TMO_MS) { s_mode = DUI_SAFE; }
            break;
        case DUI_ARMED:
            if (e == BTN_TAP) { s_mode = DUI_SAFE; }
            else if (e == BTN_HOLD) { s_mode = DUI_COUNTDOWN; stage_ms = t; }
            else if (t - stage_ms > ARMED_TMO_MS) { s_mode = DUI_SAFE; }
            break;
        case DUI_COUNTDOWN:
            if (e == BTN_TAP) { s_mode = DUI_SAFE; break; }
            if (t - stage_ms >= COUNTDOWN_MS) {
                s_abort = false; s_run_done = false; s_cur_line = 0; s_run_count++;
                s_mode = DUI_RUNNING; stage_ms = t;
                xTaskCreate(payload_task, "payload", 4096, NULL, 6, NULL);
            }
            break;
        case DUI_RUNNING:
            if (e == BTN_TAP) s_abort = true;
            if (s_run_done) { audit_write(s_abort); s_mode = DUI_DONE; stage_ms = t; }
            break;
        case DUI_DONE:
            if (e == BTN_TAP || t - stage_ms > 4000) s_mode = DUI_SAFE;
            break;
        }

        st.mode = s_mode;
        st.payload_name = s_payload_name;
        st.payload_idx = s_sel + 1;
        st.payload_count = s_npayloads > 0 ? s_npayloads : 1;
        st.total_lines = s_total_lines;
        st.cur_line = s_cur_line;
        st.usb_mounted = s_flash_mode ? false : usb_hid_mounted();
        st.dry_run = s_cfg.dry_run;
        st.layout = layout_name(s_cfg.layout);
        st.speed = speed_name(s_cfg.speed);
        st.pin_len = (int)strlen(s_cfg.arm_pin);
        st.pin_pos = s_pin_pos;
        st.pin_cur = s_pin_cur;
        st.leds = s_flash_mode ? 0 : usb_hid_leds();
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

    gpio_config_t io = { .pin_bit_mask = 1ULL << BOARD_BTN_PIN, .mode = GPIO_MODE_INPUT,
                         .pull_up_en = GPIO_PULLUP_ENABLE };
    gpio_config(&io);
    s_flash_mode = (gpio_get_level(BOARD_BTN_PIN) == 0);

    display_init();

    config_defaults(&s_cfg);
    s_sd_ok = sd_mount();
    if (s_sd_ok) {
        FILE *fp = fopen("/sdcard/DOLOS.CFG", "r");
        if (fp) {
            char cbuf[512]; int n = (int)fread(cbuf, 1, sizeof(cbuf) - 1, fp); fclose(fp);
            if (n > 0) { cbuf[n] = 0; config_parse(cbuf, &s_cfg); }
        }
        scan_payloads();
    }
    load_selected();
    usb_hid_set_speed(speed_key_delay_ms(s_cfg.speed));

    if (s_flash_mode) ESP_LOGW(TAG, "FLASH MODE (BOOT held) - USB-HID NOT started");
    else              usb_hid_init(s_cfg.usb_vid, s_cfg.usb_pid, s_cfg.usb_mfr, s_cfg.usb_product);

    ESP_LOGI(TAG, "Dolos up. mode=%s payloads=%d layout=%s speed=%s dry=%d pin=%s",
             s_flash_mode ? "FLASH" : "HID", s_npayloads, layout_name(s_cfg.layout),
             speed_name(s_cfg.speed), s_cfg.dry_run, s_cfg.arm_pin[0] ? "set" : "off");

    xTaskCreate(ui_task, "dolos_ui", 6144, NULL, 5, NULL);
}
