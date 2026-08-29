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
#include "freertos/semphr.h"
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
#include "lint.h"
#include "unicode.h"   /* os_name */
#include "menu.h"
#include "button.h"
#include "nvs.h"
#include "esp_random.h"
#include "esp_system.h"
#include "net_wifi.h"
#include "console_server.h"
#include "console_bridge.h"
#include "sbuf.h"

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
/* Payload validation: a payload with errors cannot be armed (see lint.h). */
static int          s_lint_problems;
static ducky_lint_t s_lint_first;
static volatile bool s_abort, s_run_done;
static uint32_t s_run_count;

/* PIN entry scratch */
static char s_pin_buf[9];
static int  s_pin_pos, s_pin_cur;

/* wireless console shared state */
static SemaphoreHandle_t s_lock;
static volatile bool     g_remote_fire_enabled;
static volatile int      g_remote_req;      /* 0 none, 1 arm, 2 abort */
static bool              g_wifi_up;
static bool              g_console_up;
/* Once someone has logged in, the admin password has done its job and should
 * not keep sitting on a screen anyone can pick up and read. Persisted, so it
 * stays hidden across reboots; HOLD on the console screen reveals it again,
 * and a factory reset (or NEW CREDENTIALS) brings back a fresh one to show. */
static bool              g_console_used;
/* Boot-loop guard: if the last boot crashed, come up minimal rather than
 * repeating the crash forever. A device that bricks itself on a bad setting is
 * worse than one that boots without its radio and says so. */
static bool              g_safe_boot;
static uint8_t           g_crashes;   /* consecutive crashed boots */
static uint32_t          g_boot_ms;
static char              g_admin_pw_show[20];

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

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
        /* Hidden and macOS resource-fork files are never payloads. The
         * leading-dot test only works with long filenames enabled; the
         * underscore test catches the 8.3 mangling ("._X.TXT" -> "_X~1.TXT")
         * in case a card is ever read without them. */
        if (e->d_name[0] == '.' || e->d_name[0] == '_') continue;
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
    memset(&s_lint_first, 0, sizeof(s_lint_first));
    s_lint_problems = ducky_lint(s_payload, s_cfg.layout, s_cfg.os, &s_lint_first, 1);
    if (s_lint_problems > 0)
        ESP_LOGW(TAG, "payload '%s' has %d problem(s); first at line %d: %s (arming blocked)",
                 s_payload_name, s_lint_problems, s_lint_first.line, s_lint_first.msg);
}

/* ---- boot log to the SD card ----
 *
 * This device hides its serial port by design: TinyUSB takes the USB pins for
 * HID, so ESP_LOG output - including a panic - goes to a port that no longer
 * exists. With a card present we can keep the evidence instead: every log line
 * from boot is teed to /sdcard/DOLOS_BOOT.LOG and flushed immediately, so the
 * LAST LINE IN THE FILE is the last thing that happened before a crash.
 * Logging stops once the UI is up, so it costs nothing at run time. */
static FILE *s_bootlog;
static vprintf_like_t s_prev_vprintf;

static int bootlog_vprintf(const char *fmt, va_list ap)
{
    if (s_bootlog) {
        va_list cp;
        va_copy(cp, ap);
        vfprintf(s_bootlog, fmt, cp);
        va_end(cp);
        fflush(s_bootlog);          /* a crash must not lose the last line */
    }
    return s_prev_vprintf ? s_prev_vprintf(fmt, ap) : 0;
}

static void bootlog_open(void)
{
    if (!s_sd_ok) return;
    s_bootlog = fopen("/sdcard/DOLOS_BOOT.LOG", "a");
    if (!s_bootlog) return;
    fprintf(s_bootlog, "\n===== boot: reset_reason=%d =====\n", (int)esp_reset_reason());
    fflush(s_bootlog);
    s_prev_vprintf = esp_log_set_vprintf(bootlog_vprintf);
}

static void bootlog_close(void)
{
    if (!s_bootlog) return;
    fprintf(s_bootlog, "===== boot completed, UI running =====\n");
    fflush(s_bootlog);
    if (s_prev_vprintf) esp_log_set_vprintf(s_prev_vprintf);
    fclose(s_bootlog);
    s_bootlog = NULL;
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

/* ---- button: gestures come from the host-tested recognizer in button.h ---- */
#define DOUBLE_MS 320u
static btn_state_t s_btn;
static btn_evt_t button_event(void)
{
    return button_feed(&s_btn, gpio_get_level(BOARD_BTN_PIN) == 0, now_ms());
}

/* ---- settings menu state ---- */
static int  s_menu_sel;
static bool s_cfg_dirty;     /* changed but not yet written to the card */
static bool s_info_reveal;   /* console screen: temporarily show a hidden password */

/* Write the live settings back to /sdcard/DOLOS.CFG. */
static bool config_save(void)
{
    if (!s_sd_ok) return false;
    char text[768];
    size_t n = config_write_text(&s_cfg, text, sizeof(text));
    if (n == 0) return false;
    FILE *fp = fopen("/sdcard/DOLOS.CFG", "w");
    if (!fp) return false;
    size_t w = fwrite(text, 1, n, fp);
    fclose(fp);
    if (w != n) return false;
    ESP_LOGI(TAG, "settings saved to /sdcard/DOLOS.CFG");
    return true;
}

/* Apply the settings that can take effect without a restart. */
static void config_apply_live(void)
{
    usb_hid_set_speed(speed_key_delay_ms(s_cfg.speed));
    g_remote_fire_enabled = s_cfg.remote_fire;
    load_selected();          /* re-lint: layout/OS changes can fix or break a payload */
}

/* ---- playback task ---- */
static void on_progress(int cur, int total, void *u) { (void)u; s_cur_line = cur; s_total_lines = total; }
static void payload_task(void *arg)
{
    (void)arg;
    payload_ctx_t ctx = { .abort = &s_abort, .progress = on_progress, .user = NULL,
                          .name = s_payload_name,
                          .layout = s_cfg.layout, .os = s_cfg.os, .dry_run = s_cfg.dry_run,
                          .default_delay = s_cfg.default_delay_ms };
    s_last_lines = payload_run(s_payload, &ctx);
    s_run_done = true;
    vTaskDelete(NULL);
}

/* Is the device free to start a payload?
 *
 * SAFE is the obvious one, but SENT (the screen shown for a few seconds after a
 * run), SETTINGS and CONSOLE INFO are all idle too - nothing is being typed on
 * any of them. Refusing to fire because the operator happened to be reading the
 * console screen produced "Device is busy" when nothing was busy at all.
 * Genuinely busy is PIN entry, ARMED, the countdown, and RUNNING.
 *
 * This lives in one place because the check used to exist twice, in the bridge
 * and again in the UI task, and they disagreed: the bridge accepted a request
 * from SENT that the UI task then silently discarded, so the console reported a
 * successful arm that never fired. */
static bool mode_is_idle(dui_mode_t m)
{
    return m == DUI_SAFE || m == DUI_DONE || m == DUI_MENU || m == DUI_INFO;
}

/* ---- console bridge (called from the httpd task; guarded by s_lock) ---- */
static const char *mode_str(dui_mode_t m)
{
    switch (m) { case DUI_SAFE: return "SAFE"; case DUI_PINENTRY: return "PIN";
                 case DUI_ARMED: return "ARMED"; case DUI_COUNTDOWN: return "FIRING";
                 case DUI_RUNNING: return "RUNNING"; default: return "SENT"; }
}
static bool safe_name(const char *n)
{
    if (!n || !*n || strstr(n, "..")) return false;
    for (const char *p = n; *p; p++) if (*p == '/' || *p == '\\') return false;
    return true;
}

void bridge_status_json(char *buf, size_t cap)
{
    lock();
    snprintf(buf, cap,
        "{\"mode\":\"%s\",\"payload\":\"%s\",\"idx\":%d,\"count\":%d,\"layout\":\"%s\","
        "\"speed\":\"%s\",\"dry\":%s,\"usb\":%s,\"leds\":%d,\"remote_fire\":%s,\"lines\":%d,\"cur\":%d,"
        "\"lint\":%d,\"lint_line\":%d,\"lint_msg\":\"%s\"}",
        mode_str(s_mode), s_payload_name, s_sel + 1, s_npayloads > 0 ? s_npayloads : 1,
        layout_name(s_cfg.layout), speed_name(s_cfg.speed), s_cfg.dry_run ? "true" : "false",
        (!s_flash_mode && usb_hid_mounted()) ? "true" : "false", s_flash_mode ? 0 : usb_hid_leds(),
        g_remote_fire_enabled ? "true" : "false", s_total_lines, s_cur_line,
        s_lint_problems, s_lint_first.line, s_lint_first.msg);
    unlock();
}
int bridge_list_payloads(char *out, size_t cap)
{
    lock();
    sbuf_t sb; sbuf_init(&sb, out, cap);
    sappend(&sb, "{\"payloads\":[");
    for (int i = 0; i < s_npayloads; i++)
        sappend(&sb, "%s\"%s\"", i ? "," : "", s_names[i]);
    sappend(&sb, "]}");
    unlock();
    return (int)sbuf_len(&sb);
}
int bridge_read_payload(const char *name, char *buf, size_t cap)
{
    if (!safe_name(name)) return -1;
    char path[96]; snprintf(path, sizeof(path), "/sdcard/%s", name);
    FILE *fp = fopen(path, "r"); if (!fp) return -1;
    int n = (int)fread(buf, 1, cap - 1, fp); fclose(fp);
    buf[n < 0 ? 0 : n] = 0; return n;
}
bool bridge_write_payload(const char *name, const char *data, size_t len)
{
    if (!safe_name(name) || !s_sd_ok || !ends_txt(name)) return false;
    char path[96]; snprintf(path, sizeof(path), "/sdcard/%s", name);
    FILE *fp = fopen(path, "w"); if (!fp) return false;
    size_t wrote = fwrite(data, 1, len, fp);
    fclose(fp);
    if (wrote != len) {           /* card full or removed: do not claim success */
        ESP_LOGE(TAG, "payload write truncated (%u of %u bytes)",
                 (unsigned)wrote, (unsigned)len);
        return false;
    }
    lock(); scan_payloads(); load_selected(); unlock();
    return true;
}
void bridge_get_config_text(char *buf, size_t cap)
{
    buf[0] = 0;
    if (!s_sd_ok) return;
    FILE *fp = fopen("/sdcard/DOLOS.CFG", "r"); if (!fp) return;
    char line[160];
    sbuf_t sb; sbuf_init(&sb, buf, cap);
    while (fgets(line, sizeof(line), fp) && !sbuf_truncated(&sb)) {
        const char *red = NULL;
        if (strstr(line, "wifi_pass") == line || strstr(line, "wifi_password") == line) red = "wifi_pass=***\n";
        else if (strstr(line, "admin_pass") == line || strstr(line, "admin_password") == line) red = "admin_pass=***\n";
        sappend(&sb, "%s", red ? red : line);
    }
    fclose(fp);
}
void bridge_settings_json(char *buf, size_t cap)
{
    lock();
    snprintf(buf, cap,
        "{\"layout\":\"%s\",\"os\":\"%s\",\"speed\":\"%s\","
        "\"dryrun\":%s,\"defaultdelay\":%lu,\"armpin\":%s,"
        "\"uilock\":\"%s\",\"wifi\":%s,\"remotefire\":%s,"
        "\"wifi_ssid\":\"%s\",\"sd\":%s}",
        layout_name(s_cfg.layout), os_name(s_cfg.os), speed_name(s_cfg.speed),
        s_cfg.dry_run ? "true" : "false", (unsigned long)s_cfg.default_delay_ms,
        s_cfg.arm_pin[0] ? "true" : "false", ui_lock_key(s_cfg.ui_lock),
        s_cfg.wifi_on ? "true" : "false", s_cfg.remote_fire ? "true" : "false",
        s_cfg.wifi_ssid[0] ? s_cfg.wifi_ssid : "", s_sd_ok ? "true" : "false");
    unlock();
}

bool bridge_set_setting(const char *key, const char *value)
{
    if (!key || !*key || !value) return false;
    /* Unknown key is the only real failure. Setting something to the value it
     * already holds is a perfectly ordinary request. */
    if (!config_key_known(key)) return false;
    /* Feed it through the SAME parser the config file uses, so a setting
     * changed in the console behaves identically to one written on the card -
     * and there is only one place where a key name is understood. */
    char line[160];
    int n = snprintf(line, sizeof(line), "%s=%s\n", key, value);
    if (n <= 0 || n >= (int)sizeof(line)) return false;

    lock();
    dolos_config_t before = s_cfg;
    config_parse(line, &s_cfg);
    bool changed = (memcmp(&before, &s_cfg, sizeof(s_cfg)) != 0);
    if (changed) {
        s_cfg_dirty = true;
        config_apply_live();
        g_remote_fire_enabled = s_cfg.remote_fire;
    }
    unlock();
    if (changed) {
        /* Never echo a secret's VALUE: this log is teed to the SD card, which
         * is readable on any laptop. The key name is useful, the value is not
         * worth the exposure. */
        bool secret = (strstr(key, "pass") != NULL) || (strstr(key, "pin") != NULL);
        ESP_LOGI(TAG, "console set %s=%s", key, secret ? "***" : value);
    }
    return true;                 /* the key was understood; that is the contract */
}

bool bridge_save_settings(void)
{
    lock();
    bool ok = config_save();
    if (ok) s_cfg_dirty = false;
    unlock();
    return ok;
}

bool bridge_set_config(const char *text)
{
    if (!s_sd_ok) return false;
    FILE *fp = fopen("/sdcard/DOLOS.CFG", "w"); if (!fp) return false;
    const char *p = text; char line[192];
    while (*p) {
        size_t n = 0; while (*p && *p != '\n' && n < sizeof(line) - 1) line[n++] = *p++;
        line[n] = 0; if (*p == '\n') p++;
        if (strstr(line, "wifi_pass=***") || strstr(line, "wifi_password=***"))
            fprintf(fp, "wifi_pass=%s\n", s_cfg.wifi_pass);
        else if (strstr(line, "admin_pass=***") || strstr(line, "admin_password=***"))
            fprintf(fp, "admin_pass=%s\n", s_cfg.admin_pass);
        else fprintf(fp, "%s\n", line);
    }
    fclose(fp);
    /* reload live settings (layout/speed/dry take effect now; wifi/usb need reboot) */
    lock();
    config_defaults(&s_cfg);
    FILE *rf = fopen("/sdcard/DOLOS.CFG", "r");
    if (rf) { char cb[512]; int m = (int)fread(cb, 1, sizeof(cb) - 1, rf); fclose(rf);
              if (m > 0) { cb[m] = 0; config_parse(cb, &s_cfg); } }
    usb_hid_set_speed(speed_key_delay_ms(s_cfg.speed));
    scan_payloads(); load_selected();
    unlock();
    return true;
}
int bridge_read_audit(char *buf, size_t cap)
{
    buf[0] = 0; if (!s_sd_ok) return 0;
    FILE *fp = fopen("/sdcard/DOLOS_AUDIT.LOG", "r"); if (!fp) return 0;
    fseek(fp, 0, SEEK_END); long sz = ftell(fp);
    long off = sz > (long)(cap - 1) ? sz - (long)(cap - 1) : 0;
    fseek(fp, off, SEEK_SET);
    int n = (int)fread(buf, 1, cap - 1, fp); fclose(fp);
    buf[n < 0 ? 0 : n] = 0; return n;
}
bool bridge_remote_select(const char *name)
{
    lock();
    /* payload_task is reading the very buffer load_selected() would rewrite.
     * Changing the script halfway through an injection would type a spliced
     * mixture of two payloads. */
    if (!mode_is_idle(s_mode)) { unlock(); return false; }
    bool ok = false;
    for (int i = 0; i < s_npayloads; i++) if (strcmp(s_names[i], name) == 0) { s_sel = i; load_selected(); ok = true; break; }
    unlock(); return ok;
}
bool bridge_remote_fire_enabled(void) { return g_remote_fire_enabled; }
void bridge_set_remote_fire_enabled(bool on)
{
    g_remote_fire_enabled = on;
    ESP_LOGW(TAG, "remote fire %s (via console)", on ? "ENABLED" : "disabled");
}
arm_result_t bridge_remote_arm(void)
{
    if (!g_remote_fire_enabled) return ARM_ERR_REMOTE_OFF;
    if (s_flash_mode)           return ARM_ERR_FLASH_MODE;  /* HID never started */
    lock();
    arm_result_t rc;
    /* DUI_DONE is the idle screen shown after a run; it returns to SAFE by
     * itself a few seconds later. Refusing to arm from it meant the console
     * said "refused" for several seconds after every single run, with no
     * explanation - so it counts as idle here. */
    if (s_lint_problems > 0)                            rc = ARM_ERR_LINT;
    else if (!mode_is_idle(s_mode))                     rc = ARM_ERR_BUSY;
    else { g_remote_req = 1; rc = ARM_OK; }
    unlock();
    return rc;
}
void bridge_note_console_login(void)
{
    if (g_console_used) return;
    g_console_used = true;
    nvs_handle_t h;
    if (nvs_open("dolos", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "consoleused", 1);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "console login seen - admin password no longer shown on screen");
}

void bridge_remote_abort(void) { lock(); g_remote_req = 2; unlock(); }

static void boot_guard_ok(void);   /* defined with the guard, below */

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

        /* admin-gated remote requests from the console (physical arming is
         * unchanged; remote fire only proceeds while remote_fire is enabled). */
        int rq = 0;
        if (s_lock && xSemaphoreTake(s_lock, 0) == pdTRUE) { rq = g_remote_req; g_remote_req = 0; xSemaphoreGive(s_lock); }
        if (rq == 2) {                      /* remote abort */
            if (s_mode == DUI_RUNNING)      s_abort = true;
            else if (!mode_is_idle(s_mode)) s_mode = DUI_SAFE;   /* same test as arming */
        }
        else if (rq == 1 && mode_is_idle(s_mode) && g_remote_fire_enabled && s_lint_problems == 0) {
            s_mode = DUI_COUNTDOWN; stage_ms = t;   /* same idle test as the bridge */
        }

        switch (s_mode) {
        case DUI_MENU: {
            if (e == BTN_TAP) { s_menu_sel = (s_menu_sel + 1) % MENU__COUNT; }
            else if (e == BTN_DOUBLE) { s_mode = DUI_SAFE; stage_ms = t; }
            else if (e == BTN_HOLD) {
                menu_action_t a = menu_activate(&s_cfg, (menu_item_t)s_menu_sel);
                if (a == MENU_ACT_SAVE)      { config_save(); s_cfg_dirty = false; }
                else if (a == MENU_ACT_EXIT) { s_mode = DUI_SAFE; stage_ms = t; }
                else if (a == MENU_ACT_CONSOLE_INFO) { s_mode = DUI_INFO; stage_ms = t; }
                else if (a == MENU_ACT_FACTORY) {
                    if (cv) {
                        dui_render_notice(cv, "FACTORY RESET",
                                          "ERASING SETTINGS AND", "CREDENTIALS - RESTARTING");
                        display_flush();
                        vTaskDelay(pdMS_TO_TICKS(1800));
                    }
                    dolos_factory_reset();
                }
                else if (a == MENU_ACT_NEW_CREDS) {
                    /* This restarts the device and changes the Wi-Fi key, so
                     * anyone connected is dropped. Say so on screen first -
                     * an unannounced reboot looks exactly like a crash. */
                    if (cv) {
                        dui_render_notice(cv, "NEW CREDENTIALS",
                                          "RESTARTING - REJOIN WITH", "THE NEW KEY ON SCREEN");
                        display_flush();
                        vTaskDelay(pdMS_TO_TICKS(1800));
                    }
                    /* Throw the stored secrets away and restart: the AP key is
                     * baked into the running radio, so the only honest way to
                     * apply a new one is to come up again with it. The new
                     * credentials are on the CONSOLE INFO screen after reboot. */
                    nvs_handle_t nh;
                    if (nvs_open("dolos", NVS_READWRITE, &nh) == ESP_OK) {
                        nvs_erase_key(nh, "wifi_pass");
                        nvs_erase_key(nh, "admin_pass");
                        nvs_erase_key(nh, "consoleused");   /* show the new one */
                        nvs_commit(nh);
                        nvs_close(nh);
                    }
                    ESP_LOGW(TAG, "console credentials cleared - restarting to mint new ones");
                    esp_restart();
                }
                else { s_cfg_dirty = true; lock(); config_apply_live(); unlock(); }
            }
            break;
        }
        case DUI_INFO:
            /* HOLD reveals a hidden password for as long as you stay on this
             * screen; anything else leaves, and leaving always re-hides it.
             * (This handler was lost in an edit, which is why HOLD=SHOW did
             * nothing: the flag was read by the UI but never set.) */
            if (e == BTN_HOLD)      { s_info_reveal = !s_info_reveal; }
            else if (e != BTN_NONE) { s_mode = DUI_MENU; s_info_reveal = false; stage_ms = t; }
            break;
        case DUI_SAFE:
            /* ui_lock is a level: MENU hides the settings screen, FULL also
             * stops the payload being switched, so a device left in the field
             * does exactly the one job it was configured for. Neither level
             * touches arming, firing or the console. */
            if (e == BTN_DOUBLE && s_cfg.ui_lock == UI_LOCK_OFF && !s_flash_mode) {
                s_mode = DUI_MENU; s_menu_sel = 0; stage_ms = t;
            }
            else if (e == BTN_TAP && s_npayloads > 1 && s_cfg.ui_lock < UI_LOCK_FULL) {
                s_sel = (s_sel + 1) % s_npayloads; load_selected();
            }
            else if (e == BTN_HOLD && !s_flash_mode && s_lint_problems == 0) {
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
                /* If the task cannot be created the payload never runs, and the
                 * old code still moved to RUNNING - where it waited on a
                 * completion flag nothing would ever set. The device sat on
                 * "RUNNING" until it was power-cycled. Only enter RUNNING once
                 * the task actually exists. */
                if (xTaskCreate(payload_task, "payload", 6144, NULL, 6, NULL) == pdPASS) {
                    s_mode = DUI_RUNNING; stage_ms = t;
                } else {
                    ESP_LOGE(TAG, "could not start the payload task - out of memory");
                    s_mode = DUI_SAFE; stage_ms = t;
                }
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
        st.wifi_on = g_wifi_up;
        st.wifi_ssid = g_wifi_up ? net_wifi_ssid() : NULL;
        st.remote_fire_enabled = g_remote_fire_enabled;
        /* Ran long enough without crashing: clear the boot-loop counter. */
        static bool cleared = false;
        if (!cleared && (t - g_boot_ms) > 15000u) { cleared = true; boot_guard_ok(); }

        st.menu_sel = s_menu_sel;
        st.cfg = &s_cfg;
        st.ui_lock = s_cfg.ui_lock;
        st.safe_boot = g_safe_boot;
        st.degraded = (g_crashes > 0);
        st.console_up = g_console_up;
        /* Hide the console password once it has been used. This assignment was
         * missing, so the flag was permanently false and the password stayed on
         * screen no matter how many times someone logged in. */
        st.admin_pw_masked = g_console_used && !s_info_reveal;
        st.wifi_key = g_wifi_up ? s_cfg.wifi_pass : NULL;
        st.admin_user = s_cfg.admin_user[0] ? s_cfg.admin_user : "admin";
        st.lint_problems = s_lint_problems;
        st.lint_line = s_lint_first.line;
        st.lint_msg = s_lint_first.msg[0] ? s_lint_first.msg : NULL;
        st.admin_pw = g_wifi_up ? (g_admin_pw_show[0] ? g_admin_pw_show : s_cfg.admin_pass) : NULL;
        st.countdown = 3 - (int)((t - stage_ms) / 1000);
        if (st.countdown < 1) st.countdown = 1;
        st.anim++;

        if (cv) { dui_render(cv, &st); display_flush(); }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* Decide whether this boot should be a reduced "safe boot".
 *
 * A crash reboots the ESP32, and if the cause is deterministic (a bad config, a
 * radio that cannot allocate) the device simply crashes again - a boot loop the
 * operator cannot break out of, because every setting lives on an SD card the
 * device may not even have. So a crash is counted in NVS, and after two in a
 * row the next boot skips the optional subsystems and says SAFE BOOT on screen.
 * A successful run clears the count (see boot_guard_ok). */
static void boot_guard_begin(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    bool crashed = (r == ESP_RST_PANIC || r == ESP_RST_TASK_WDT ||
                    r == ESP_RST_INT_WDT || r == ESP_RST_WDT);
    uint8_t count = 0;
    nvs_handle_t h;
    if (nvs_open("dolos", NVS_READWRITE, &h) == ESP_OK) {
        nvs_get_u8(h, "crashes", &count);          /* absent -> stays 0 */
        count = crashed ? (uint8_t)(count + 1) : 0;
        nvs_set_u8(h, "crashes", count);
        nvs_commit(h);
        nvs_close(h);
    }
    g_crashes = count;
    g_safe_boot = (count >= 2);
    uint8_t used = 0;
    if (nvs_open("dolos", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, "consoleused", &used) == ESP_OK) g_console_used = (used != 0);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "reset reason=%d crashed=%d consecutive=%u -> %s",
             (int)r, (int)crashed, count, g_safe_boot ? "SAFE BOOT" : "normal boot");
}

/* Called once the device has run long enough to call this boot a success. */
static void boot_guard_ok(void)
{
    nvs_handle_t h;
    if (nvs_open("dolos", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "crashes", 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* Wipe this device back to a clean state: generated console secrets and the
 * saved configuration, then restart so fresh secrets are minted.
 *
 * This is the ONLY reversal Dolos can honestly offer. Flash encryption and
 * Secure Boot are eFuse-based and cannot be undone by any password - see
 * docs/HARDENING.md. What an authorised person CAN do is return the device to
 * factory state, which is what this does. */
void dolos_factory_reset(void)
{
    nvs_handle_t h;
    if (nvs_open("dolos", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    if (s_sd_ok) remove("/sdcard/DOLOS.CFG");     /* payloads are left alone */
    ESP_LOGW(TAG, "FACTORY RESET - restarting with fresh credentials");
    esp_restart();
}

/* Generate a readable random secret: no 0/O/1/I/l, because these are read off a
 * 1.14" screen and typed into a phone. */
static void gen_secret(char *out, size_t len)
{
    /* 32 characters: digits and capitals minus 0/O and 1/I, the pairs no font
     * can separate. Legibility of the REST is the font's job, not the
     * alphabet's - shrinking the character set to work around a hard-to-read
     * typeface would cost real entropy per character for a cosmetic reason.
     * 32^16 is about 80 bits for the Wi-Fi key and 32^14 about 70 for the
     * console password. */
    static const char AB[] = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ";
    for (size_t i = 0; i < len; i++) out[i] = AB[esp_random() % (sizeof(AB) - 1)];
    out[len] = 0;
}

/* Give this device its own AP passphrase and admin password on first boot and
 * remember them in NVS.
 *
 * There is deliberately NO shipped default credential: every unit that flashes
 * this firmware would share it, and the whole point of having a screen is that
 * the device can show you a unique secret instead. Anything set in DOLOS.CFG
 * wins, so an operator can still pin their own. */
static void ensure_credentials(void)
{
    /* NOTE ON ENTROPY.
     *
     * esp_random() is only a TRUE random number generator while the RF
     * subsystem is running, and credentials are minted here, before the radio
     * starts. The documented remedy - bootloader_random_enable() around the
     * generation - was tried and REVERTED: it reconfigures SAR-ADC/RTC state
     * immediately before esp_wifi_init(), and the radio then failed to start,
     * which the boot-loop guard turned into a device that came up without its
     * console. A working device beats a marginally better key.
     *
     * The proper fix is to mint credentials once the RF subsystem is already
     * up, rather than borrowing a different entropy source before it. That is
     * tracked as follow-up work; until then this is a known limitation. */
    nvs_handle_t h;
    if (nvs_open("dolos", NVS_READWRITE, &h) != ESP_OK) return;
    bool dirty = false;
    size_t len;

    if (!s_cfg.wifi_pass[0]) {
        len = sizeof(s_cfg.wifi_pass);
        if (nvs_get_str(h, "wifi_pass", s_cfg.wifi_pass, &len) != ESP_OK) {
            gen_secret(s_cfg.wifi_pass, 16);   /* 32^16 ~= 80 bits */
            if (nvs_set_str(h, "wifi_pass", s_cfg.wifi_pass) != ESP_OK)
                ESP_LOGE(TAG, "could not store the Wi-Fi key - it will change on reboot");
            dirty = true;
        }
    }
    if (!s_cfg.admin_pass[0]) {
        len = sizeof(s_cfg.admin_pass);
        if (nvs_get_str(h, "admin_pass", s_cfg.admin_pass, &len) != ESP_OK) {
            gen_secret(s_cfg.admin_pass, 14);  /* 32^14 ~= 70 bits, behind lockout */
            if (nvs_set_str(h, "admin_pass", s_cfg.admin_pass) != ESP_OK)
                ESP_LOGE(TAG, "could not store the admin password - it will change on reboot");
            dirty = true;
        }
    }
    if (dirty) nvs_commit(h);
    nvs_close(h);
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
    button_init(&s_btn, HOLD_MS, DOUBLE_MS);

    boot_guard_begin();
    g_boot_ms = now_ms();

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
    bootlog_open();
    load_selected();
    usb_hid_set_speed(speed_key_delay_ms(s_cfg.speed));

    if (s_flash_mode) ESP_LOGW(TAG, "FLASH MODE (BOOT held) - USB-HID NOT started");
    else              usb_hid_init(s_cfg.usb_vid, s_cfg.usb_pid, s_cfg.usb_mfr, s_cfg.usb_product);

    /* Wireless console: WPA2 SoftAP + secure HTTP console.
     *
     * It runs in FLASH MODE too. FLASH MODE means USB-HID never started, so the
     * device physically cannot type - a console on it can manage settings and
     * payloads but cannot fire anything, which is strictly safe. It also keeps
     * the USB serial port alive while the radio is up, which is the only way to
     * see a fault in the Wi-Fi path at all (in normal operation TinyUSB owns
     * the USB pins and panics print to a port that no longer exists). */
    s_lock = xSemaphoreCreateMutex();
    g_remote_fire_enabled = s_cfg.remote_fire;
    ensure_credentials();
    /* Degrade in stages rather than all at once.
     *
     * A crash used to cost the whole radio, which threw away the console as
     * well and told us nothing about which of the two was at fault. Now the
     * first crash drops only the HTTP console and keeps the access point, and
     * only a second one drops the radio entirely. The screen names the stage,
     * so a single power cycle says where the fault is. */
    if (s_cfg.wifi_on && g_crashes < 2) {
        if (net_wifi_start_ap(s_cfg.wifi_ssid, s_cfg.wifi_pass)) {
            g_wifi_up = true;
            if (g_crashes == 0) {
                if (console_server_start(s_cfg.admin_user, s_cfg.admin_pass, s_cfg.remote_fire)) {
                    g_console_up = true;
                    if (!s_cfg.admin_pass[0])
                        strncpy(g_admin_pw_show, console_admin_password(), sizeof(g_admin_pw_show) - 1);
                }
            } else {
                ESP_LOGW(TAG, "previous boot crashed - access point only, no HTTP console");
            }
        }
    }

    ESP_LOGI(TAG, "Dolos up. mode=%s payloads=%d layout=%s speed=%s dry=%d pin=%s",
             s_flash_mode ? "FLASH" : "HID", s_npayloads, layout_name(s_cfg.layout),
             speed_name(s_cfg.speed), s_cfg.dry_run, s_cfg.arm_pin[0] ? "set" : "off");

    /* Without the UI task there is no screen and no button: the device would
     * sit on the splash looking bricked, with nothing anywhere saying why. It
     * is the last thing started, so if memory is this tight the honest thing is
     * to say so loudly rather than pretend the device came up. */
    if (xTaskCreate(ui_task, "dolos_ui", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "FATAL: could not start the UI task - out of memory. "
                      "The screen and button will not respond.");
    }
    vTaskDelay(pdMS_TO_TICKS(3000));   /* capture anything the UI logs early */
    bootlog_close();
}
