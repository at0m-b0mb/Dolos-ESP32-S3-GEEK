#include "usb_hid.h"
#include "hid_keys.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"

static const char *TAG = "usb_hid";
static volatile uint8_t s_leds = 0; /* host LED state: exfil return channel */

/* Report IDs for the composite HID device. */
enum { RID_KEYBOARD = 1, RID_MOUSE, RID_CONSUMER };

/* Keyboard + mouse + consumer-control in one HID interface. */
static const uint8_t s_hid_report[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(RID_KEYBOARD)),
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(RID_MOUSE)),
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(RID_CONSUMER)),
};

/* Composite: keyboard + mass storage, which is what ATTACKMODE HID STORAGE
 * describes. The MSC interface is always in the descriptor because interfaces
 * cannot be added after enumeration - what changes at run time is whether the
 * drive reports a medium. Until a payload asks for storage the host is told the
 * slot is empty, so it shows nothing and mounts nothing. */
#define DOLOS_CFG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_MSC_DESC_LEN)
#define ITF_NUM_HID  0
#define ITF_NUM_MSC  1
static const uint8_t s_config[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, DOLOS_CFG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 4, HID_ITF_PROTOCOL_NONE, sizeof(s_hid_report),
                       0x81, 16, 1),
    /* string index 5, OUT 0x02 / IN 0x82, 64-byte packets */
    TUD_MSC_DESCRIPTOR(ITF_NUM_MSC, 5, 0x02, 0x82, 64),
};

/* String + device descriptors are filled from config so the USB identity can be
 * set by the operator for an authorized allow-list test. Defaults are generic
 * "Dolos". */
static char s_mfr[24]  = "Dolos";
static char s_prod[32] = "Dolos HID Keyboard";
static const char *s_strdesc[6] = { (const char[]){0x09, 0x04}, s_mfr, s_prod, "000001",
                                   "Dolos HID", "Dolos Storage" };

static tusb_desc_device_t s_dev = {
    .bLength = sizeof(tusb_desc_device_t), .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200, .bDeviceClass = 0x00, .bDeviceSubClass = 0x00, .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A, .idProduct = 0x4004,        /* Espressif default */
    .bcdDevice = 0x0100, .iManufacturer = 1, .iProduct = 2, .iSerialNumber = 3,
    .bNumConfigurations = 1,
};

/* ---- host operating-system detection ------------------------------------
 *
 * Asking the operator to set the target OS by hand is a trap: get it wrong and
 * every non-ASCII character silently turns to rubbish, because Windows, Linux
 * and macOS each type Unicode a completely different way. The host tells us
 * what it is during enumeration, if we listen.
 *
 * The signals, in order of how much they are worth:
 *
 *  - Windows and Linux both push the keyboard LED state to a new keyboard
 *    shortly after they configure it. macOS does NOT: it leaves the lock LEDs
 *    alone until something changes them. No LED report is therefore strong
 *    evidence of a Mac, and it is the distinction that matters most here
 *    because macOS is the odd one out for Unicode.
 *  - Linux binds usbhid with an explicit SET_PROTOCOL; Windows normally does
 *    not bother for a device that reports the boot protocol already.
 *  - macOS reads the initial input report over the control pipe (GET_REPORT),
 *    which the other two rarely do.
 *
 * None of this is a standard, so the verdict carries its evidence with it, is
 * printed on the screen and written to the log, and can always be overridden
 * by setting `os=` in the config. A wrong guess must never be a silent one. */
static volatile uint32_t s_report_desc_reqs;
static volatile uint32_t s_get_report_reqs;
static volatile bool     s_set_protocol_seen;
static volatile bool     s_led_report_seen;
static volatile uint32_t s_mount_ms;
static char s_detect_why[96];

/* macOS is decided by the ABSENCE of a message, so we have to wait long enough
 * to be sure it is not merely late. Windows and Linux send theirs within a few
 * tens of milliseconds of configuring the device. */
#define OS_DETECT_SETTLE_MS 900u

void tud_mount_cb(void)
{
    s_mount_ms = (uint32_t)(esp_timer_get_time() / 1000);
    s_led_report_seen = false;
    s_set_protocol_seen = false;
    s_get_report_reqs = 0;
}

void tud_hid_set_protocol_cb(uint8_t instance, uint8_t protocol)
{
    (void)instance; (void)protocol;
    s_set_protocol_seen = true;
}

usb_host_os_t usb_hid_detect_os(void)
{
    if (!tud_mounted() || s_mount_ms == 0) {
        snprintf(s_detect_why, sizeof(s_detect_why), "no host attached");
        return USB_HOST_UNKNOWN;
    }
    uint32_t since = (uint32_t)(esp_timer_get_time() / 1000) - s_mount_ms;
    if (since < OS_DETECT_SETTLE_MS && !s_led_report_seen) {
        snprintf(s_detect_why, sizeof(s_detect_why), "still listening (%ums)", (unsigned)since);
        return USB_HOST_UNKNOWN;          /* too early to call it a Mac */
    }
    /* The raw signals go into the message verbatim. These heuristics are not a
     * standard and the only way to improve them is to see what real machines
     * actually do, so the evidence is always reported beside the verdict. */
    char ev[64];
    snprintf(ev, sizeof(ev), "led=%d proto=%d getrep=%u desc=%u t=%ums",
             s_led_report_seen ? 1 : 0, s_set_protocol_seen ? 1 : 0,
             (unsigned)s_get_report_reqs, (unsigned)s_report_desc_reqs,
             (unsigned)since);
    if (!s_led_report_seen) {
        snprintf(s_detect_why, sizeof(s_detect_why), "no LED report; %s", ev);
        return USB_HOST_MAC;
    }
    if (s_set_protocol_seen) {
        snprintf(s_detect_why, sizeof(s_detect_why), "LED+SET_PROTOCOL; %s", ev);
        return USB_HOST_LINUX;
    }
    snprintf(s_detect_why, sizeof(s_detect_why), "LED, no SET_PROTOCOL; %s", ev);
    return USB_HOST_WINDOWS;
}

const char *usb_hid_detect_why(void)  { return s_detect_why[0] ? s_detect_why : "not evaluated"; }
bool     usb_hid_saw_led_report(void) { return s_led_report_seen; }
uint32_t usb_hid_desc_requests(void)  { return s_report_desc_reqs; }

/* --- TinyUSB HID callbacks --- */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance; s_report_desc_reqs++; return s_hid_report;
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance; (void)report_id; (void)type; (void)buffer; (void)reqlen;
    s_get_report_reqs++;              /* macOS reads the initial report this way */
    return 0;
}

/* The host writes the keyboard LED state here (CapsLock/NumLock/ScrollLock).
 * We keep it as a 3-bit return channel a payload/operator can read on-screen. */
void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t type,
                           uint8_t const *buffer, uint16_t bufsize)
{
    (void)instance; (void)report_id;
    if (type == HID_REPORT_TYPE_OUTPUT && bufsize >= 1) {
        s_leds = buffer[0];
        s_led_report_seen = true;     /* the signal that says "not a Mac" */
    }
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
        .string_descriptor_count = 6,
        .external_phy = false,
        .configuration_descriptor = s_config,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&cfg));
    ESP_LOGI(TAG, "USB HID (kbd+mouse+consumer) up as %04X:%04X '%s' (SAFE)",
             s_dev.idVendor, s_dev.idProduct, s_prod);
}

bool usb_hid_ready(void)   { return tud_mounted() && tud_hid_ready(); }
bool usb_hid_mounted(void) { return tud_mounted(); }

/* ---- self-clocked report delivery -------------------------------------
 *
 * A fixed delay between keystrokes is a guess about a machine we have never
 * met: too short and characters vanish, too long and every payload crawls. The
 * USB stack already knows the answer. tud_hid_ready() is false while a report
 * is queued and true again once the HOST HAS POLLED IT, so waiting for that
 * edge paces us at exactly the host's own rate - never faster than it can
 * consume, never slower than it can go.
 *
 * A keystroke is therefore not "send, then sleep N milliseconds", it is:
 *
 *     wait until the endpoint is free        (previous report collected)
 *     queue this report                      (retry if the stack refuses)
 *     wait until the endpoint is free again  (THIS report collected)
 *
 * By the time we return, the host demonstrably has it. That is what makes
 * every speed profile accurate rather than only the slowest one: the profile
 * now chooses an extra settle margin, not whether pacing happens at all.
 *
 * (The previous design could not work regardless of its numbers: the scheduler
 * ran at 100 Hz, so pdMS_TO_TICKS(5) rounded DOWN TO ZERO and "balanced" had no
 * pacing whatsoever. The tick is 1 kHz now, and this path no longer depends on
 * it for sub-millisecond timing.)
 */
#define HID_WAIT_US 400000u          /* 400 ms - vastly longer than any poll */

static uint32_t s_drops;
static uint16_t s_last_retries;
static bool     s_last_ok = true;
static uint32_t s_guard_us;          /* extra settle time from the speed profile */

static uint32_t now_us(void) { return (uint32_t)esp_timer_get_time(); }

/* Wait for the endpoint to drain. Polls finely at first, then yields so the
 * idle task still runs and the watchdog stays fed. */
static bool wait_endpoint(void)
{
    uint32_t start = now_us();
    while (!tud_hid_ready()) {
        if (!tud_mounted()) return false;
        uint32_t waited = now_us() - start;
        if (waited > HID_WAIT_US) return false;
        if (waited > 2000) vTaskDelay(1);          /* past 2 ms: stop spinning */
        else               esp_rom_delay_us(50);
    }
    return true;
}

/* The settle margin must YIELD, not spin.
 *
 * esp_rom_delay_us() is a busy loop: it blocks the task without releasing the
 * CPU. The "reliable" profile asks for 8 ms per keystroke, so a 200-character
 * payload spent over a second and a half spinning - starving the idle task the
 * watchdog feeds from, and every other task of equal or lower priority on this
 * core. Whole milliseconds go through vTaskDelay (the tick is 1 kHz, so this is
 * exact); only the sub-millisecond remainder is spun. */
static void guard(void)
{
    if (!s_guard_us) return;
    uint32_t ms = s_guard_us / 1000u;
    uint32_t us = s_guard_us % 1000u;
    if (ms) vTaskDelay(pdMS_TO_TICKS(ms));
    if (us) esp_rom_delay_us(us);
}

static bool kb_report(uint8_t mods, const uint8_t *keys)
{
    for (int attempt = 0; attempt < 200; attempt++) {
        if (!wait_endpoint()) break;
        if (tud_hid_keyboard_report(RID_KEYBOARD, mods, keys)) {
            s_last_retries = (uint16_t)attempt;
            /* The queue accepted it; delivery is only proven when the endpoint
             * drains again. Ignoring this return reported a keystroke as sent
             * that the host had not taken - the injection log then said "sent"
             * for characters that never arrived. */
            bool taken = wait_endpoint();
            s_last_ok = taken;
            if (!taken) {
                s_drops++;
                ESP_LOGW(TAG, "report queued but never collected by the host");
            }
            guard();
            return taken;
        }
        esp_rom_delay_us(200);
    }
    s_last_retries = 200;
    s_last_ok = false;
    s_drops++;
    ESP_LOGW(TAG, "keystroke dropped - host never took the report (%lu total)",
             (unsigned long)s_drops);
    return false;
}

static bool mouse_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
    for (int attempt = 0; attempt < 200; attempt++) {
        if (!wait_endpoint()) break;
        if (tud_hid_mouse_report(RID_MOUSE, buttons, dx, dy, wheel, 0)) {
            wait_endpoint();
            guard();
            return true;
        }
        esp_rom_delay_us(200);
    }
    s_drops++;
    return false;
}

uint32_t usb_hid_drops(void)        { return s_drops; }
uint16_t usb_hid_last_retries(void) { return s_last_retries; }
bool     usb_hid_last_ok(void)      { return s_last_ok; }
uint8_t  usb_hid_leds(void)         { return s_leds; }

void usb_hid_set_speed(uint8_t guard_ms)
{
    /* Pacing comes from the host; this is only an additional settle margin for
     * hosts that acknowledge a report before they have finished acting on it.
     * Zero is now a legitimate setting. */
    s_guard_us = (uint32_t)guard_ms * 1000u;
}

bool usb_hid_wait_mounted(uint32_t timeout_ms)
{
    uint32_t waited = 0;
    while (!tud_mounted() && waited < timeout_ms) { vTaskDelay(pdMS_TO_TICKS(10)); waited += 10; }
    return tud_mounted();
}

bool usb_hid_wait_host_ready(uint32_t timeout_ms)
{
    if (!tud_mounted()) return false;
    const uint8_t before = s_leds;
    usb_hid_tap(0, HID_KEY_CAPS);                 /* ask the OS a question */

    uint32_t waited = 0;
    while (s_leds == before && waited < timeout_ms) { vTaskDelay(pdMS_TO_TICKS(5)); waited += 5; }
    const bool echoed = (s_leds != before);

    usb_hid_tap(0, HID_KEY_CAPS);                 /* put it back how we found it */
    uint32_t back = 0;
    while (s_leds != before && back < 400) { vTaskDelay(pdMS_TO_TICKS(5)); back += 5; }

    ESP_LOGI(TAG, "host-ready handshake: %s after %lums",
             echoed ? "acknowledged" : "no LED echo (host may not report synchronously)",
             (unsigned long)waited);
    return echoed;
}

/* ---- public keystroke API (all of it self-clocked via kb_report) ---- */

void usb_hid_tap(uint8_t mods, uint8_t key)
{
    uint8_t keys[6] = { key, 0, 0, 0, 0, 0 };
    kb_report(mods, key ? keys : NULL);
    kb_report(0, NULL);
}

/* A NON-modifier key held down across reports - DuckyScript's "HOLD SPACE".
 *
 * The parser understood these all along and the player threw the key away:
 * only the modifier byte was ever sent, so "HOLD SHIFT" worked and "HOLD SPACE"
 * pressed nothing at all and said nothing about it. Keeping the key here means
 * every report sent while a HOLD is active carries it, which is what "held"
 * means to the host - the key stays down until RELEASE. */
static uint8_t s_hold_key;

void usb_hid_hold(uint8_t mods)
{
    uint8_t keys[6] = { s_hold_key, 0, 0, 0, 0, 0 };
    kb_report(mods, s_hold_key ? keys : NULL);
}

void usb_hid_hold_key(uint8_t mods, uint8_t key)
{
    s_hold_key = key;
    usb_hid_hold(mods);
}

void usb_hid_key(uint8_t tap_mods, uint8_t held_after, uint8_t key)
{
    /* The same usage must not appear twice in one report. */
    uint8_t keys[6] = { key, (key != s_hold_key) ? s_hold_key : 0, 0, 0, 0, 0 };
    kb_report(tap_mods, (key || s_hold_key) ? keys : NULL);
    uint8_t still[6] = { s_hold_key, 0, 0, 0, 0, 0 };
    kb_report(held_after, s_hold_key ? still : NULL);  /* key up, HOLD stays down */
}

void usb_hid_release(void) { s_hold_key = 0; kb_report(0, NULL); }

void usb_hid_mouse(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
    mouse_report(buttons, dx, dy, wheel);
    if (buttons) mouse_report(0, 0, 0, 0);        /* a click: release it */
}

void usb_hid_consumer(uint16_t usage)
{
    if (!wait_endpoint()) return;
    tud_hid_report(RID_CONSUMER, &usage, 2);
    wait_endpoint();
    uint16_t zero = 0;
    if (!wait_endpoint()) return;
    tud_hid_report(RID_CONSUMER, &zero, 2);
    wait_endpoint();
    guard();
}
