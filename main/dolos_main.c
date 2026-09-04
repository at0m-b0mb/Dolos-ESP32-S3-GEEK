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
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
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
#include "dscript.h"
#include "unicode.h"   /* os_name */
#include "menu.h"
#include "button.h"
#include "nvs.h"
#include "esp_random.h"
#include "esp_system.h"
#include "net_wifi.h"
#include "console_server.h"
#include "console_bridge.h"
#include "usb_msc.h"

/* set by a payload waiting on WAIT_FOR_BUTTON_PRESS (see payload.c) */
extern volatile bool g_wait_button;
#include "sbuf.h"

static const char *TAG = "dolos";

#define HOLD_MS       1200u
#define ARMED_TMO_MS  8000u
#define PIN_TMO_MS    15000u
#define COUNTDOWN_MS  3000u
/* 32, not 16: the test set alone is 17 files and a real engagement card holds
 * more. The scan silently stopped at the limit, so extra payloads simply did
 * not exist as far as the picker was concerned - a confusing way to lose a
 * file. 32 x 64 bytes of names is 2 KB, which is affordable. */
#define MAX_PAYLOADS  32
/* Consecutive crashed boots before the optional subsystems are skipped once.
 * Four, not two: a single unlucky pair of resets should never cost you the
 * console, and the retry below means even this is temporary. */
#define SAFE_BOOT_AFTER 4

static sdmmc_card_t *s_card;      /* retained for the storage window */
static dui_mode_t   s_mode = DUI_SAFE;
static bool         s_flash_mode, s_sd_ok;
static dolos_config_t s_cfg;

static char  s_names[MAX_PAYLOADS][64];
static int   s_npayloads, s_sel;
/* 6 KB was not a limit anyone had chosen; it was just the size of an array, and
 * a bigger file was read up to it and NUL-terminated mid-line without a word.
 *
 * The replacement must come off the PSRAM HEAP, exactly as dscript_alloc()
 * does. EXT_RAM_BSS_ATTR was tried and is a trap: it compiles to nothing unless
 * CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY is enabled, which this project
 * deliberately leaves off - so the array landed in internal DRAM and took
 * 26 KB from the one pool Wi-Fi, TinyUSB and the SD card all need. The device
 * boot-looped and refused to fire. Internal RAM is the scarce resource here;
 * nothing large may sit in it by accident. */
#define PAYLOAD_MAX 32768
static char  *s_payload_buf;      /* PSRAM heap, allocated once at boot */
static size_t s_payload_cap;
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

static bool wifi_bring_up(void);   /* defined with the boot path, below */
static bool mode_is_idle(dui_mode_t m);
/* Set when a reload was refused because a payload was running; applied the
 * moment the run ends. */
static bool s_reload_pending;

/* Destructive menu actions ask twice.
 *
 * NEW CREDENTIALS and FACTORY RESET both throw away the Wi-Fi key, so the
 * operator is locked out of their own console until they read the new one off
 * the screen. Both sat on a menu you step through with a single button, one
 * hold away from happening by accident - which is exactly how it happened.
 * The first hold arms the action and says what it will destroy; only a second,
 * deliberate hold carries it out, and any other press cancels. */
static menu_action_t s_confirm;          /* MENU_ACT_NONE = nothing pending */
static const char *s_confirm_t1, *s_confirm_t2, *s_confirm_t3;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* Adopt the host we actually detected, when the operator asked us to.
 *
 * s_cfg.os stays the single value everything reads - the player, the linter and
 * the screen - so there is no second notion of "the real OS" to get out of step
 * with the first. os_auto records that it was detected rather than chosen, and
 * that is what gets written back to the card. Detection returning UNKNOWN
 * changes nothing: the previous value stands. */
static void load_selected(void);
static void ensure_checked(void);
static void heap_checkpoint(const char *stage);
static bool sd_mount(void);
static void scan_payloads(void);

/* Notice a card that was inserted AFTER boot.
 *
 * sd_mount() used to run once, at startup. Boot the device with no card, push
 * one in afterwards, and nothing happened: the payload list stayed empty and
 * the button cycled a list of one built-in demo, with nothing on screen to say
 * the card had been ignored. People plug the card in when they need it, not
 * before. Retried only while idle, and only when there is no card - mounting
 * underneath a running payload is never safe. */
static void sd_hotplug_check(void)
{
    if (s_sd_ok) return;
    if (!sd_mount()) return;
    s_sd_ok = true;
    ESP_LOGW(TAG, "SD card inserted - reading it now");
    FILE *fp = fopen("/sdcard/DOLOS.CFG", "r");
    if (fp) {
        char cbuf[512]; int n = (int)fread(cbuf, 1, sizeof(cbuf) - 1, fp); fclose(fp);
        if (n > 0) { cbuf[n] = 0; config_parse(cbuf, &s_cfg); }
    }
    usb_hid_set_speed(speed_key_delay_ms(s_cfg.speed));
    scan_payloads();
    load_selected();
}

static void os_detect_apply(void)
{
    if (!s_cfg.os_auto || s_flash_mode) return;
    usb_host_os_t d = usb_hid_detect_os();
    target_os_t want;
    switch (d) {
        case USB_HOST_WINDOWS: want = OS_WINDOWS; break;
        case USB_HOST_LINUX:   want = OS_LINUX;   break;
        case USB_HOST_MAC:     want = OS_MAC;     break;
        default: return;                       /* not enough evidence yet */
    }
    /* Log the first settled verdict as well as any later change: a verdict
     * that merely agrees with the current setting is still the thing we want
     * to see in the log when the answer turns out to be wrong. */
    static bool logged;
    if (!logged || want != s_cfg.os) {
        logged = true;
        ESP_LOGW(TAG, "host detected as %s (%s)", os_name(want), usb_hid_detect_why());
    }
    if (want == s_cfg.os) return;
    s_cfg.os = want;
    load_selected();      /* the linter judges typability against the OS */
}
static void lock(void)   { if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { if (s_lock) xSemaphoreGive(s_lock); }

/* ---- SD ---- */
static bool sd_mount(void)
{
    spi_bus_config_t bus = { .sclk_io_num = BOARD_SD_PIN_SCLK, .mosi_io_num = BOARD_SD_PIN_MOSI,
                             .miso_io_num = BOARD_SD_PIN_MISO, .quadwp_io_num = -1,
                             .quadhd_io_num = -1, .max_transfer_sz = 4000 };
    /* The bus can only be set up once for the life of the process, and this is
     * now called again whenever a card is inserted - so ALREADY DONE is a
     * success here, not a failure. Treating it as one meant a retry could never
     * mount anything. */
    esp_err_t be = spi_bus_initialize(BOARD_SD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (be != ESP_OK && be != ESP_ERR_INVALID_STATE) return false;
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = BOARD_SD_SPI_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = BOARD_SD_PIN_CS; slot.host_id = BOARD_SD_SPI_HOST;
    /* 6, not 4: the boot log now holds one handle for the whole session, and a
     * run can have the payload, the audit log and the injection log open at the
     * same time. Running out shows up as a mystery "cannot open" much later. */
    esp_vfs_fat_sdmmc_mount_config_t mc = { .format_if_mount_failed = false, .max_files = 6 };
    if (esp_vfs_fat_sdspi_mount("/sdcard", &host, &slot, &mc, &s_card) != ESP_OK) return false;
    /* Keep the card handle: the mass-storage window reads and writes sectors
     * directly, below the filesystem, so it needs the device rather than the
     * mount point. */
    return true;
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
    if (s_npayloads >= MAX_PAYLOADS)
        ESP_LOGW(TAG, "payload list is full at %d - any further files on the card "
                      "are not listed", MAX_PAYLOADS);
}

/* Handed to the DuckyScript engine so a long parse cannot starve the system.
 *
 * One tick is enough: it puts this task at the back of the queue for its
 * priority and lets the idle task run, which is what the task watchdog is
 * actually watching for. Linting a 16 KB payload on the UI task at priority 5
 * held the CPU long enough to trip it - the device froze, then rebooted, then
 * came up in safe boot. */
static void engine_yield(void) { vTaskDelay(1); }

/* Which selection the expensive check was last done for. -1 = none. */
static int s_checked_sel = -1;

/* Read and lint the selected payload, unless that was already done for this
 * selection. Called where a verdict is genuinely needed - before arming - and
 * nowhere else, so cycling the list costs nothing. */
static void ensure_checked(void)
{
    if (s_checked_sel == s_sel && s_total_lines > 0) return;
    load_selected();
}

/* Choosing a payload is FREE.
 *
 * Reading and linting a script is the most expensive thing this device does,
 * and it was being done on every selection - every button tap while cycling
 * the list, from the UI task, above the priority of the task the watchdog
 * watches. Selecting is now just a name; the work happens once, when it
 * matters, and is remembered. */
static void select_payload(int i)
{
    if (i < 0 || i >= s_npayloads) return;
    s_sel = i;
    s_payload_name = s_names[i];
    s_checked_sel = -1;                  /* unknown, not "clean" */
    s_total_lines = 0;
    s_lint_problems = 0;
    memset(&s_lint_first, 0, sizeof(s_lint_first));
}

static void load_selected(void)
{
    /* NEVER rewrite the buffer the payload task is reading.
     *
     * payload_run() walks s_payload in place. Reloading underneath it - which
     * uploading a payload, changing a setting or saving the config all did -
     * splices two scripts together mid-injection and types the result into
     * someone's machine. bridge_remote_select() had this guard; three other
     * callers did not, so it lives here instead, where the danger actually is.
     * The reload is not lost, only deferred until the run finishes. */
    if (!mode_is_idle(s_mode)) {
        s_reload_pending = true;
        ESP_LOGW(TAG, "payload reload deferred: a run is in progress");
        return;
    }
    s_reload_pending = false;
    if (s_npayloads > 0) {
        char path[96];
        snprintf(path, sizeof(path), "/sdcard/%s", s_names[s_sel]);
        FILE *fp = s_payload_buf ? fopen(path, "r") : NULL;
        if (fp) {
            /* Read through a small INTERNAL bounce buffer, never straight into
             * PSRAM.
             *
             * The payload buffer lives in PSRAM, and FATFS reads run through
             * the SD card's SPI driver, which is DMA-driven. Handing a DMA
             * path a PSRAM destination is a hazard on this chip - and the SPI
             * bus here is configured with max_transfer_sz = 4000, far below the
             * size being asked for. The heap was healthy at the checkpoint
             * before this call and the device died before the next one, which
             * is exactly the shape of a DMA write landing somewhere it should
             * not. Filesystem data now only ever lands in internal memory, and
             * we copy it across ourselves. */
            static char bounce[1024];
            size_t want = s_payload_cap - 1, got = 0;
            for (;;) {
                size_t chunk = want - got;
                if (chunk == 0) break;
                if (chunk > sizeof(bounce)) chunk = sizeof(bounce);
                size_t r = fread(bounce, 1, chunk, fp);
                if (r == 0) break;
                memcpy(s_payload_buf + got, bounce, r);
                got += r;
                if (r < chunk) break;              /* end of file */
            }
            int n = (int)got;
            /* Is there MORE of the file than we just read? Half a payload that
             * looks like a whole one is the worst outcome: it types a script
             * that ends mid-line and reports success. */
            int extra = fgetc(fp);
            fclose(fp);
            if (extra != EOF) {
                ESP_LOGE(TAG, "payload '%s' is larger than %u bytes - refusing to run half of it",
                         s_names[s_sel], (unsigned)s_payload_cap);
                s_payload = "REM payload too large for this device\n";
                s_payload_name = s_names[s_sel];
                s_total_lines = 1;
                memset(&s_lint_first, 0, sizeof(s_lint_first));
                s_lint_first.line = 1;
                snprintf(s_lint_first.msg, sizeof(s_lint_first.msg),
                         "payload is bigger than %u KB", (unsigned)(s_payload_cap / 1024));
                s_lint_problems = 1;      /* blocks arming, and says why */
                return;
            }
            if (n > 0) { s_payload_buf[n] = 0; s_payload = s_payload_buf;
                         s_payload_name = s_names[s_sel]; }
            heap_checkpoint("payload_read");
        }
    } else {
        s_payload = DOLOS_DEMO_PAYLOAD;
        s_payload_name = "demo";
    }
    s_total_lines = payload_count_lines(s_payload);
    memset(&s_lint_first, 0, sizeof(s_lint_first));
    heap_checkpoint("lint_begin");
    uint32_t t0 = now_ms();
    s_lint_problems = ducky_lint(s_payload, s_cfg.layout, s_cfg.os, &s_lint_first, 1);
    uint32_t lint_ms = now_ms() - t0;
    if (lint_ms > 250)
        ESP_LOGW(TAG, "linting '%s' (%d lines) took %lums",
                 s_payload_name, s_total_lines, (unsigned long)lint_ms);
    heap_checkpoint("lint_end");
    s_checked_sel = s_sel;
    if (s_lint_problems > 0)
        ESP_LOGW(TAG, "payload '%s' has %d problem(s); first at line %d: %s (arming blocked)",
                 s_payload_name, s_lint_problems, s_lint_first.line, s_lint_first.msg);
}

/* ---- boot log to the SD card ----
 *
 * This device hides its serial port by design: TinyUSB takes the USB pins, so
 * ESP_LOG output - including a panic - goes to a port that no longer exists.
 * With a card present the lines are kept instead.
 *
 * THE HOOK MUST NEVER TOUCH THE FILESYSTEM. It is called on whatever task
 * happened to log, and some of those have small stacks: esp_timer runs on
 * ~3.5 KB, and vfprintf() through FATFS needs more than it has left. Doing the
 * write inline overflowed that stack and jumped through a poisoned frame -
 * a crash the log existed to diagnose, caused by the log itself. The core dump
 * caught it: crashed task 'esp_timer', pc 0xfffffffb, stack full of 0xa5a5.
 *
 * So the hook only formats into a small buffer and posts it to a queue, which
 * costs a couple of hundred bytes of stack and never blocks. One writer task,
 * with a stack of its own, does the file I/O. A full queue drops the line -
 * losing a log line is always better than taking the device down. */
#define BOOTLOG_QLEN   24
#define BOOTLOG_LINE  160
#define BOOTLOG_CAP  (192 * 1024)

static FILE *s_bootlog;
static vprintf_like_t s_prev_vprintf;
static QueueHandle_t  s_bootlog_q;
static long           s_bootlog_bytes;
static volatile bool  s_bootlog_on;

static int bootlog_vprintf(const char *fmt, va_list ap)
{
    if (s_bootlog_on && s_bootlog_q) {
        char line[BOOTLOG_LINE];
        va_list cp;
        va_copy(cp, ap);
        int n = vsnprintf(line, sizeof(line), fmt, cp);   /* bounded, no FILE */
        va_end(cp);
        if (n > 0) xQueueSend(s_bootlog_q, line, 0);      /* never waits */
    }
    return s_prev_vprintf ? s_prev_vprintf(fmt, ap) : 0;
}

static void bootlog_task(void *arg)
{
    (void)arg;
    char line[BOOTLOG_LINE];
    for (;;) {
        if (xQueueReceive(s_bootlog_q, line, portMAX_DELAY) != pdTRUE) continue;
        if (!s_bootlog || !s_bootlog_on) continue;
        size_t len = strlen(line);
        fwrite(line, 1, len, s_bootlog);
        fflush(s_bootlog);                    /* a crash must not lose it */
        if ((s_bootlog_bytes += (long)len) > BOOTLOG_CAP) s_bootlog_on = false;
    }
}

static void bootlog_open(void)
{
    if (!s_sd_ok) return;
    /* Opt-in. Mirroring the log to the card means writing from whatever task
     * emitted the line, and that is what kept overflowing small stacks. The
     * core dump records crashes without touching the filesystem at all. */
    if (!s_cfg.bootlog) return;
    s_bootlog = fopen("/sdcard/DOLOS_BOOT.LOG", "a");
    if (!s_bootlog) return;
    fprintf(s_bootlog, "\n===== boot: reset_reason=%d =====\n", (int)esp_reset_reason());
    fflush(s_bootlog);

    s_bootlog_q = xQueueCreate(BOOTLOG_QLEN, BOOTLOG_LINE);
    if (!s_bootlog_q) { fclose(s_bootlog); s_bootlog = NULL; return; }
    /* 4 KB of its own, so file I/O never runs on a caller's stack. */
    if (xTaskCreate(bootlog_task, "bootlog", 4096, NULL, 1, NULL) != pdPASS) {
        vQueueDelete(s_bootlog_q); s_bootlog_q = NULL;
        fclose(s_bootlog); s_bootlog = NULL;
        return;
    }
    s_bootlog_on = true;
    s_prev_vprintf = esp_log_set_vprintf(bootlog_vprintf);
}

/* Mark the end of the boot sequence. Nothing is closed and nothing is freed:
 * the hook and the writer task stay, so a panic AFTER boot is recorded too. */
static void bootlog_boot_done(void)
{
    if (!s_bootlog_on) return;
    ESP_LOGI(TAG, "===== boot completed, UI running =====");
}

/* ---- audit log ---- */
static void audit_write(bool aborted)
{
    if (!s_sd_ok) return;
    /* Two writers on one filesystem corrupts it. While the host has the
     * storage window, the card is not ours to write to. */
    if (usb_msc_exposed()) { ESP_LOGW(TAG, "audit entry skipped: host holds the storage"); return; }
    FILE *fp = fopen("/sdcard/DOLOS_AUDIT.LOG", "a");
    if (!fp) return;
    /* "sent" is claimed only when something was actually typed. Zero executed
     * lines is a FAILURE, and the reason goes in the log beside it. */
    const char *why = payload_last_failure();
    const char *result = aborted ? "aborted" : (s_last_lines > 0 ? "sent" : "FAILED");
    fprintf(fp, "run=%lu up_ms=%lu payload=%s lines=%d result=%s dry=%d layout=%s speed=%s",
            (unsigned long)s_run_count, (unsigned long)now_ms(), s_payload_name, s_last_lines,
            result, s_cfg.dry_run ? 1 : 0,
            layout_name(s_cfg.layout), speed_name(s_cfg.speed));
    if (s_last_lines <= 0 && !aborted) fprintf(fp, " reason=%s", why ? why : "payload produced no lines");
    fputc('\n', fp);
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
    /* The Wi-Fi setting used to change nothing until the next boot, while the
     * console screen reported the radio as off and told the operator to enable
     * it in settings - where it already was. Apply it here instead. */
    if (s_cfg.wifi_on && !g_wifi_up)      wifi_bring_up();
    else if (!s_cfg.wifi_on && g_wifi_up) { net_wifi_stop_ap(); g_wifi_up = false; }

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

/* Escape a string so it is safe inside a JSON double-quoted value.
 *
 * Payload names, SSIDs and lint messages were interpolated raw. A Wi-Fi name
 * containing a quote or a backslash - both perfectly legal in an SSID - made
 * the status document unparseable, so the console's JSON.parse threw and the
 * whole page went blank with nothing to explain it. */
static void json_str(char *out, size_t cap, const char *in)
{
    size_t o = 0;
    if (cap == 0) return;
    for (const char *p = in ? in : ""; *p && o + 7 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c == '\r') { out[o++] = '\\'; out[o++] = 'r'; }
        else if (c == '\t') { out[o++] = '\\'; out[o++] = 't'; }
        else if (c < 0x20)  { o += (size_t)snprintf(out + o, cap - o, "\\u%04x", c); }
        else out[o++] = (char)c;
    }
    out[o < cap ? o : cap - 1] = 0;
}

/* The UPSTREAM Wi-Fi password is a credential for someone else's network, and
 * it earns the same treatment as ours: the device's own flash, never the
 * removable card. Nothing persisted it at all, so an uplink set up in the
 * console worked until the next power cycle and then quietly was not there. */
static void secret_store(const char *key, const char *val)
{
    nvs_handle_t h;
    if (nvs_open("dolos", NVS_READWRITE, &h) != ESP_OK) return;
    if (val && *val) nvs_set_str(h, key, val);
    else             nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
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
    char e_payload[80], e_lint[80], e_ssid[80];
    json_str(e_payload, sizeof(e_payload), s_payload_name);
    json_str(e_lint,    sizeof(e_lint),    s_lint_first.msg);
    json_str(e_ssid,    sizeof(e_ssid),    net_wifi_sta_ssid());
    snprintf(buf, cap,
        "{\"mode\":\"%s\",\"payload\":\"%s\",\"idx\":%d,\"count\":%d,\"layout\":\"%s\","
        "\"speed\":\"%s\",\"dry\":%s,\"usb\":%s,\"leds\":%d,\"remote_fire\":%s,\"lines\":%d,\"cur\":%d,"
        "\"lint\":%d,\"lint_line\":%d,\"lint_msg\":\"%s\","
        "\"uplink\":%s,\"uplink_ssid\":\"%s\",\"uplink_ip\":\"%s\"}",
        mode_str(s_mode), e_payload, s_sel + 1, s_npayloads > 0 ? s_npayloads : 1,
        layout_name(s_cfg.layout), speed_name(s_cfg.speed), s_cfg.dry_run ? "true" : "false",
        (!s_flash_mode && usb_hid_mounted()) ? "true" : "false", s_flash_mode ? 0 : usb_hid_leds(),
        g_remote_fire_enabled ? "true" : "false", s_total_lines, s_cur_line,
        s_lint_problems, s_lint_first.line, e_lint,
        net_wifi_sta_connected() ? "true" : "false",
        e_ssid, net_wifi_sta_ip());
    unlock();
}
int bridge_list_payloads(char *out, size_t cap)
{
    lock();
    sbuf_t sb; sbuf_init(&sb, out, cap);
    sappend(&sb, "{\"payloads\":[");
    for (int i = 0; i < s_npayloads; i++) {
        char e[80];
        json_str(e, sizeof(e), s_names[i]);   /* a filename is not trusted text */
        sappend(&sb, "%s\"%s\"", i ? "," : "", e);
    }
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
/* Is this config line a secret?
 *
 * Matching the KEY for "pass" rather than listing field names means a field
 * added later is protected by DEFAULT instead of by someone remembering to
 * extend a list. That is exactly what went wrong: uplink_pass arrived with the
 * Internet-uplink feature and was neither redacted when the console read the
 * config nor stripped when the console wrote it to the card - so an upstream
 * network password could be shown in a browser and written in clear text to
 * removable media, which is the one thing this project says it never does. */
static bool config_line_is_secret(const char *line)
{
    char key[40]; size_t n = 0;
    while (line[n] && line[n] != '=' && n < sizeof(key) - 1) {
        char c = line[n];
        key[n] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
        n++;
    }
    key[n] = 0;
    return strstr(key, "pass") != NULL;
}

void bridge_get_config_text(char *buf, size_t cap)
{
    buf[0] = 0;
    if (!s_sd_ok) return;
    FILE *fp = fopen("/sdcard/DOLOS.CFG", "r"); if (!fp) return;
    char line[160];
    sbuf_t sb; sbuf_init(&sb, buf, cap);
    while (fgets(line, sizeof(line), fp) && !sbuf_truncated(&sb)) {
        if (config_line_is_secret(line)) {
            /* Keep the key visible so the operator can see it is SET, and
             * never the value. */
            char key[40]; size_t k = 0;
            while (line[k] && line[k] != '=' && k < sizeof(key) - 1) { key[k] = line[k]; k++; }
            key[k] = 0;
            sappend(&sb, "%s=***\n", key);
        } else {
            sappend(&sb, "%s", line);
        }
    }
    fclose(fp);
}
void bridge_settings_json(char *buf, size_t cap)
{
    lock();
    /* An SSID is user-supplied text going into a JSON document: escape it, or
     * a network named with a quote makes the whole settings page unparseable.
     * The uplink PASSWORD is deliberately never sent - the field is write-only,
     * exactly like the console password. */
    char e_ap[80], e_up[80];
    json_str(e_ap, sizeof(e_ap), s_cfg.wifi_ssid);
    json_str(e_up, sizeof(e_up), s_cfg.sta_ssid);
    snprintf(buf, cap,
        "{\"layout\":\"%s\",\"os\":\"%s\",\"speed\":\"%s\","
        "\"dryrun\":%s,\"defaultdelay\":%lu,\"armpin\":%s,"
        "\"uilock\":\"%s\",\"wifi\":%s,\"remotefire\":%s,"
        "\"wifi_ssid\":\"%s\",\"sd\":%s,"
        /* These three exist because the console binds controls to them. Without
         * them the Internet-uplink fields could never show what was set: they
         * read back as undefined and rendered blank forever. */
        "\"uplink\":%s,\"uplink_ssid\":\"%s\","
        "\"bootlog\":%s,\"storage\":%s}",
        layout_name(s_cfg.layout), os_name(s_cfg.os), speed_name(s_cfg.speed),
        s_cfg.dry_run ? "true" : "false", (unsigned long)s_cfg.default_delay_ms,
        s_cfg.arm_pin[0] ? "true" : "false", ui_lock_key(s_cfg.ui_lock),
        s_cfg.wifi_on ? "true" : "false", s_cfg.remote_fire ? "true" : "false",
        e_ap, s_sd_ok ? "true" : "false",
        s_cfg.sta_on ? "true" : "false", e_up,
        s_cfg.bootlog ? "true" : "false", s_cfg.msc_enabled ? "true" : "false");
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
        if (strcmp(before.sta_pass, s_cfg.sta_pass) != 0)
            secret_store("sta_pass", s_cfg.sta_pass);
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
        line[n] = 0;
        /* If the line was longer than the buffer, discard its tail rather than
         * letting the remainder become a second, bogus setting on the next
         * pass round the loop. */
        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
        /* Secrets are NEVER written to the card - not even to "restore" a
         * redacted line. An SD card is removable and readable on any laptop,
         * and this path would have put the live Wi-Fi key and console password
         * back into a plain text file, undoing the protection every other
         * writer respects. The values stay in NVS; the line is dropped. */
        if (config_line_is_secret(line)) continue;
        fprintf(fp, "%s\n", line);
    }
    fclose(fp);
    /* reload live settings (layout/speed/dry take effect now; wifi/usb need reboot) */
    lock();
    /* Keep the secrets across the reload.
     *
     * config_defaults() clears every field, and the file no longer contains the
     * Wi-Fi key or console password because we deliberately stopped writing
     * them there. Without this, saving the config silently emptied both in
     * memory - and the next attempt to bring the radio up would fail its
     * ">= 8 character passphrase" check with no obvious reason why. They live
     * in NVS; carry them over the reload. */
    char keep_wifi[sizeof(s_cfg.wifi_pass)], keep_admin[sizeof(s_cfg.admin_pass)];
    snprintf(keep_wifi,  sizeof(keep_wifi),  "%s", s_cfg.wifi_pass);
    snprintf(keep_admin, sizeof(keep_admin), "%s", s_cfg.admin_pass);

    config_defaults(&s_cfg);
    FILE *rf = fopen("/sdcard/DOLOS.CFG", "r");
    if (rf) { char cb[512]; int m = (int)fread(cb, 1, sizeof(cb) - 1, rf); fclose(rf);
              if (m > 0) { cb[m] = 0; config_parse(cb, &s_cfg); } }
    /* the file wins only if it actually set one */
    if (!s_cfg.wifi_pass[0])  snprintf(s_cfg.wifi_pass,  sizeof(s_cfg.wifi_pass),  "%s", keep_wifi);
    if (!s_cfg.admin_pass[0]) snprintf(s_cfg.admin_pass, sizeof(s_cfg.admin_pass), "%s", keep_admin);

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
    for (int i = 0; i < s_npayloads; i++)
        if (strcmp(s_names[i], name) == 0) { select_payload(i); ok = true; break; }
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
    if (!mode_is_idle(s_mode))                          rc = ARM_ERR_BUSY;
    else if ((ensure_checked(), s_lint_problems > 0))   rc = ARM_ERR_LINT;
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
        /* A payload waiting on WAIT_FOR_BUTTON_PRESS is released by any press,
         * and that press does nothing else - it belongs to the payload. */
        if (g_wait_button && e != BTN_NONE) { g_wait_button = false; e = BTN_NONE; }
        uint32_t t = now_ms();

        /* Re-check which machine we are plugged into.
         *
         * Throttled, and only while idle: adopting a new target OS re-lints the
         * payload, which must never happen underneath a run. tud_mount_cb
         * resets the evidence on every re-plug, so moving the device from a Mac
         * to a PC is noticed without a reboot. */
        static uint32_t last_detect;
        if (t - last_detect > 250 && mode_is_idle(s_mode)) {
            last_detect = t;
            lock(); os_detect_apply(); unlock();
        }
        /* A card pushed in after boot. Polled slowly: probing an empty slot
         * costs an SPI transaction, and nobody inserts a card twice a second. */
        static uint32_t last_sd;
        if (!s_sd_ok && t - last_sd > 1500 && mode_is_idle(s_mode)) {
            last_sd = t;
            lock(); sd_hotplug_check(); unlock();
        }

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
            /* Anything that is not the confirming hold cancels a pending
             * destructive action. Walking away must never carry it out. */
            if (s_confirm != MENU_ACT_NONE && e != BTN_NONE && e != BTN_HOLD) {
                s_confirm = MENU_ACT_NONE;
                e = BTN_NONE;                    /* the press was the cancel */
            }
            if (e == BTN_TAP) { s_menu_sel = (s_menu_sel + 1) % MENU__COUNT; }
            else if (e == BTN_DOUBLE) { s_mode = DUI_SAFE; stage_ms = t; s_confirm = MENU_ACT_NONE; }
            else if (e == BTN_HOLD) {
                menu_action_t a;
                bool just_armed = false;
                if (s_confirm != MENU_ACT_NONE) {
                    a = s_confirm;               /* the second, deliberate hold */
                    s_confirm = MENU_ACT_NONE;
                } else {
                    a = menu_activate(&s_cfg, (menu_item_t)s_menu_sel);
                    if (a == MENU_ACT_FACTORY || a == MENU_ACT_NEW_CREDS) {
                        s_confirm = a;
                        s_confirm_t1 = (a == MENU_ACT_FACTORY) ? "FACTORY RESET" : "NEW CREDENTIALS";
                        s_confirm_t2 = (a == MENU_ACT_FACTORY)
                                       ? "ERASES SETTINGS AND THE WI-FI KEY"
                                       : "CHANGES THE WI-FI KEY - YOU WILL BE";
                        s_confirm_t3 = (a == MENU_ACT_FACTORY)
                                       ? "HOLD AGAIN TO CONFIRM - TAP CANCELS"
                                       : "SIGNED OUT. HOLD AGAIN - TAP CANCELS";
                        a = MENU_ACT_NONE;       /* not this time */
                        just_armed = true;       /* and nothing was changed */
                    }
                }
                if (a == MENU_ACT_SAVE)      { config_save(); s_cfg_dirty = false; }
                else if (a == MENU_ACT_EXIT) { s_mode = DUI_SAFE; stage_ms = t; s_confirm = MENU_ACT_NONE; }
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
                        nvs_erase_key(nh, "wifi_ssid");   /* a new name as well */
                        nvs_erase_key(nh, "wifi_pass");
                        nvs_erase_key(nh, "admin_pass");
                        nvs_erase_key(nh, "consoleused");   /* show the new one */
                        nvs_commit(nh);
                        nvs_close(nh);
                    }
                    ESP_LOGW(TAG, "console credentials cleared - restarting to mint new ones");
                    esp_restart();
                }
                /* Arming a confirmation changed no setting, so it must not mark
                 * the config dirty or re-apply anything. */
                else if (!just_armed) { s_cfg_dirty = true; lock(); config_apply_live(); unlock(); }
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
                /* Under the lock: the console task reloads through the very
                 * same path, and ducky_lint() keeps an 8 KB line buffer and a
                 * parser as statics. Two tasks in there at once corrupt both the
                 * payload text and the lint verdict - and that verdict is what
                 * decides whether the device is allowed to arm. */
                lock();
                select_payload((s_sel + 1) % s_npayloads);
                unlock();
            }
            else if (e == BTN_HOLD && !s_flash_mode) {
                lock(); ensure_checked(); unlock();
                if (s_lint_problems != 0) break;      /* the screen says why */
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
            if (e != BTN_NONE) { s_mode = DUI_SAFE; break; }   /* any press cancels */
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
            /* ANY press stops it, not just a clean tap.
             *
             * Only BTN_TAP did, so someone trying to stop an injection by
             * holding the button - which is what people actually do when they
             * want something to stop NOW - was ignored, and a hold during a
             * long WAIT looked like the device had frozen. A stop control must
             * not be fussy about how it is pressed. */
            if (e != BTN_NONE) s_abort = true;
            if (s_run_done) {
                audit_write(s_abort);
                s_mode = DUI_DONE; stage_ms = t;
                /* the run is over: it is safe to pick up anything the console
                 * changed while it was going */
                if (s_reload_pending) { lock(); load_selected(); unlock(); }
            }
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
        st.storage_shared = usb_msc_exposed();
        st.storage_part = usb_msc_partition();
        st.wifi_key = g_wifi_up ? s_cfg.wifi_pass : NULL;
        st.admin_user = s_cfg.admin_user[0] ? s_cfg.admin_user : "admin";
        st.run_failed  = (s_mode == DUI_DONE && s_last_lines <= 0 && !s_abort);
        st.run_fail_msg = payload_last_failure();
        st.lint_problems = s_lint_problems;
        st.lint_line = s_lint_first.line;
        st.lint_msg = s_lint_first.msg[0] ? s_lint_first.msg : NULL;
        st.admin_pw = g_wifi_up ? (g_admin_pw_show[0] ? g_admin_pw_show : s_cfg.admin_pass) : NULL;
        st.countdown = 3 - (int)((t - stage_ms) / 1000);
        if (st.countdown < 1) st.countdown = 1;
        st.anim++;

        if (cv) {
            if (s_confirm != MENU_ACT_NONE)
                dui_render_notice(cv, s_confirm_t1, s_confirm_t2, s_confirm_t3);
            else
                dui_render(cv, &st);
            display_flush();
        }
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
/* Why the chip restarted, in words. A panic writes a core dump and a backtrace;
 * a brownout or a hardware watchdog writes NOTHING, so those two look exactly
 * like a device that "just restarted" with no evidence anywhere. Naming the
 * reason is the difference between debugging and guessing. */
static const char *reset_reason_name(esp_reset_reason_t r)
{
    switch (r) {
        case ESP_RST_POWERON:  return "power-on";
        case ESP_RST_EXT:      return "external reset";
        case ESP_RST_SW:       return "software restart";
        case ESP_RST_PANIC:    return "PANIC (core dump written)";
        case ESP_RST_INT_WDT:  return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT:      return "other watchdog";
        case ESP_RST_DEEPSLEEP:return "deep sleep";
        case ESP_RST_BROWNOUT: return "BROWNOUT (supply dipped)";
        case ESP_RST_SDIO:     return "SDIO";
        default:               return "unknown";
    }
}
const char *g_reset_reason = "";

static void boot_guard_begin(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    g_reset_reason = reset_reason_name(r);
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
    /* The guard exists so a repeatable crash cannot brick the device. But it
     * was too eager and too permanent: two crashes disabled the radio, and
     * every later boot inherited that verdict, so a device that was fine came
     * up crippled with no way back except erasing NVS.
     *
     * Now it takes FOUR consecutive crashes, and it always tries the radio
     * again on the next boot afterwards - the count is cleared as soon as it
     * degrades once. Safe boot is a single cautious retry, not a sentence. */
    if (count >= SAFE_BOOT_AFTER) {
        g_safe_boot = true;
        count = 0;                    /* try again next time, don't stay off */
        nvs_handle_t h2;
        if (nvs_open("dolos", NVS_READWRITE, &h2) == ESP_OK) {
            nvs_set_u8(h2, "crashes", 0);
            nvs_commit(h2);
            nvs_close(h2);
        }
    } else {
        g_safe_boot = false;
    }
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
/* Bring the access point (and, on a clean boot, the console) up. Shared by the
 * boot path and the settings toggle so the two can never drift apart. The HTTP
 * server is started once per boot: its handlers are registered globally, so
 * re-registering them on a second call would be a leak, not a restart. */
static bool wifi_bring_up(void)
{
    if (g_wifi_up) return true;
    /* Internal (DMA-capable) RAM is the scarce resource on this board, and the
     * radio is the biggest consumer of it. Record what was left before it
     * starts: a crash during bring-up looks identical whether the cause is a
     * bug or simply no memory, and this is the number that tells them apart. */
    ESP_LOGW(TAG, "internal heap before Wi-Fi: %u free, %u largest block",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    if (!net_wifi_start_ap(s_cfg.wifi_ssid, s_cfg.wifi_pass)) return false;
    ESP_LOGW(TAG, "internal heap after  Wi-Fi: %u free, %u largest block",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    g_wifi_up = true;
    if (!g_console_up && g_crashes == 0) {
        if (console_server_start(s_cfg.admin_user, s_cfg.admin_pass, s_cfg.remote_fire)) {
            g_console_up = true;
            if (!s_cfg.admin_pass[0])
                strncpy(g_admin_pw_show, console_admin_password(), sizeof(g_admin_pw_show) - 1);
        }
    }
    return true;
}

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

    /* A RANDOM network name, not one derived from the MAC.
     *
     * The SSID was "Dolos-" plus two bytes of the MAC address, which is fixed
     * for the life of the board: the same name for ever, and one that anyone
     * who has seen this device once can recognise and tie back to a specific
     * unit. Minted once, kept in NVS so it survives a reboot, and replaced
     * along with the rest by NEW CREDENTIALS. Setting wifi_ssid in DOLOS.CFG
     * still overrides it. */
    if (!s_cfg.wifi_ssid[0]) {
        len = sizeof(s_cfg.wifi_ssid);
        if (nvs_get_str(h, "wifi_ssid", s_cfg.wifi_ssid, &len) != ESP_OK) {
            char sfx[9];
            gen_secret(sfx, 8);                       /* 32^8 ~= 40 bits */
            snprintf(s_cfg.wifi_ssid, sizeof(s_cfg.wifi_ssid), "Dolos-%s", sfx);
            if (nvs_set_str(h, "wifi_ssid", s_cfg.wifi_ssid) != ESP_OK)
                ESP_LOGE(TAG, "could not store the SSID - it will change on reboot");
            dirty = true;
        }
    }
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
    /* Remembered, never generated: an absent uplink password is a valid state,
     * so this only restores one the operator set. */
    if (!s_cfg.sta_pass[0]) {
        len = sizeof(s_cfg.sta_pass);
        nvs_get_str(h, "sta_pass", s_cfg.sta_pass, &len);
    }
    if (dirty) nvs_commit(h);
    nvs_close(h);
}

/* Bisect the boot: which stage corrupts the heap?
 *
 * The crash is never where the damage was done - it lands in whichever malloc
 * or heap walk next touches the poisoned block, which so far has been TinyUSB's
 * task stack and then the heap logging in wifi_bring_up(). With poisoning on,
 * checking integrity after each stage names the stage responsible. Results are
 * buffered until the card is mounted, because the earliest stages run before
 * there is anywhere to write them. */
/* The stage a crash happened in, carried ACROSS the reset.
 *
 * Writing the result to the card cannot work when the thing being reported is
 * the reason fopen() is about to fail - the last checkpoint died inside fopen,
 * so the very line naming the culprit was the one that never got written. RTC
 * memory survives a panic reset, so the marker is stamped BEFORE anything
 * risky, and the next boot reports what the previous one was doing. */
RTC_NOINIT_ATTR static uint32_t s_rtc_magic;
RTC_NOINIT_ATTR static char     s_rtc_stage[24];
RTC_NOINIT_ATTR static uint32_t s_rtc_stack_left;
#define RTC_STAGE_MAGIC 0xD0105A1Eu

static char   s_heaplog[640];
static size_t s_heaplog_n;
static bool   s_booting = true;   /* load_selected() runs later too; only log boot */

static void heap_checkpoint(const char *stage)
{
    if (!s_booting) return;
    /* Stamped first, so it survives even if this function is what dies. */
    snprintf(s_rtc_stage, sizeof(s_rtc_stage), "%s", stage);
    s_rtc_stack_left = (uint32_t)uxTaskGetStackHighWaterMark(NULL);
    s_rtc_magic = RTC_STAGE_MAGIC;
    /* No integrity walk here.
     *
     * heap_caps_check_integrity() does not return false on a damaged heap - it
     * faults inside it - so calling it every few lines of boot guaranteed a
     * panic instead of reporting one. What is still worth recording costs
     * nothing and cannot fail: how far boot got, and how much stack was left. */
    bool ok_int = true, ok_ext = true, ok = true;
    (void)ok_int; (void)ok_ext;
    if (s_heaplog_n < sizeof(s_heaplog) - 1) {
        int w = snprintf(s_heaplog + s_heaplog_n, sizeof(s_heaplog) - s_heaplog_n,
                         "  %-14s reached, stack_left=%u\n", stage,
                         (unsigned)s_rtc_stack_left);
        if (w > 0 && (size_t)w < sizeof(s_heaplog) - s_heaplog_n) s_heaplog_n += (size_t)w;
    }
    ESP_LOGW(TAG, "boot stage: %s (stack left %u)", stage, (unsigned)s_rtc_stack_left);
    (void)ok;
    /* Deliberately does NOT write to the card here.
     *
     * Writing a diagnostic from the main task while the boot-log task is also
     * writing means two tasks in FATFS at once, which is one of the things
     * being investigated. A measurement must not perturb what it measures.
     * Everything is buffered and flushed once, by heap_log_flush(). */
}

/* Called once boot is over, when nothing else is competing for the card. */
static void heap_log_flush(void)
{
    if (!s_sd_ok || !s_heaplog_n) return;
    FILE *f = fopen("/sdcard/DOLOS_HEAP.LOG", "a");
    if (f) { fwrite(s_heaplog, 1, s_heaplog_n, f); fclose(f); }
    s_heaplog_n = 0;
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
    ducky_set_yield(engine_yield);   /* before anything parses a payload */
    heap_checkpoint("display_init");

    config_defaults(&s_cfg);
    s_sd_ok = sd_mount();
    heap_checkpoint("sd_mount");
    if (s_sd_ok) {
        FILE *fp = fopen("/sdcard/DOLOS.CFG", "r");
        if (fp) {
            char cbuf[512]; int n = (int)fread(cbuf, 1, sizeof(cbuf) - 1, fp); fclose(fp);
            if (n > 0) { cbuf[n] = 0; config_parse(cbuf, &s_cfg); }
        }
        scan_payloads();
    }
    /* One line per boot, always. This is deliberately NOT the opt-in boot log:
     * it is three facts that cost nothing and answer "what happened last time",
     * which neither the screen nor the serial port can tell us once TinyUSB
     * owns the USB pins. */
    if (s_sd_ok) {
        /* A DIFFERENT file from the opt-in boot log: that one holds a whole
         * session of ESP_LOG output and is held open for the run, and two
         * handles appending to one file interleave into nonsense. */
        FILE *bf = fopen("/sdcard/DOLOS_RESET.LOG", "a");
        if (bf) {
            if (s_rtc_magic == RTC_STAGE_MAGIC)
                fprintf(bf, "  ^ previous boot died in stage '%s' with %u bytes of stack left\n",
                        s_rtc_stage, (unsigned)s_rtc_stack_left);
            s_rtc_magic = 0;
            fprintf(bf, "boot: reason=%s crashes=%u safe=%d internal_free=%u largest=%u\n",
                    g_reset_reason, (unsigned)g_crashes, g_safe_boot ? 1 : 0,
                    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            fclose(bf);
        }
    }
    bootlog_open();
    heap_checkpoint("bootlog_open");
    /* Work out which partition could be shared, but do not share it: nothing is
     * exposed until a payload asks with ATTACKMODE STORAGE. */
    if (s_sd_ok) usb_msc_init(s_card, s_cfg.msc_partition);
    heap_checkpoint("usb_msc_init");
    /* The payload text: PSRAM by preference, a small internal buffer only if
     * there is no PSRAM at all. Never a large static in internal DRAM. */
    s_payload_buf = heap_caps_malloc(PAYLOAD_MAX, MALLOC_CAP_SPIRAM);
    s_payload_cap = s_payload_buf ? PAYLOAD_MAX : 0;
    if (!s_payload_buf) {
        s_payload_buf = heap_caps_malloc(6144, MALLOC_CAP_INTERNAL);
        s_payload_cap = s_payload_buf ? 6144 : 0;
        ESP_LOGW(TAG, "no PSRAM for the payload buffer - falling back to %u bytes",
                 (unsigned)s_payload_cap);
    }
    ESP_LOGW(TAG, "payload buffer: %u bytes at %p", (unsigned)s_payload_cap, s_payload_buf);
    heap_checkpoint("payload_alloc");
    load_selected();
    usb_hid_set_speed(speed_key_delay_ms(s_cfg.speed));
    heap_checkpoint("load_selected");

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
    heap_checkpoint("usb_hid_init");
    g_remote_fire_enabled = s_cfg.remote_fire;
    ensure_credentials();
    heap_checkpoint("ensure_credentials");
    /* Degrade in stages rather than all at once.
     *
     * A crash used to cost the whole radio, which threw away the console as
     * well and told us nothing about which of the two was at fault. Now the
     * first crash drops only the HTTP console and keeps the access point, and
     * only a second one drops the radio entirely. The screen names the stage,
     * so a single power cycle says where the fault is. */
    if (s_cfg.wifi_on && !g_safe_boot) {
        if (wifi_bring_up()) {
            if (g_crashes != 0) {
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
    heap_checkpoint("before_ui_task");
    s_booting = false;
    heap_log_flush();
    if (xTaskCreate(ui_task, "dolos_ui", 8192, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "FATAL: could not start the UI task - out of memory. "
                      "The screen and button will not respond.");
    }
    vTaskDelay(pdMS_TO_TICKS(3000));   /* capture anything the UI logs early */
    bootlog_boot_done();
}
