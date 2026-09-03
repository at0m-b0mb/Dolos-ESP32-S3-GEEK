#include "net_wifi.h"
#include <string.h>
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "esp_timer.h"

static const char *TAG = "net_wifi";
static char s_ssid[33];
static bool s_active;

/* ---- station side ---- */
static char s_sta_ssid[33];
static char s_sta_ip[16];
static bool s_sta_on, s_sta_conn;

/* Retrying a join is deferred onto a timer, NEVER slept for in place.
 *
 * The reconnect used to vTaskDelay(3000) inside the event handler, which runs
 * on the shared system event task - so a wrong upstream password blocked every
 * other event on the device for three seconds at a time, over and over: the
 * access point the operator is actually using, its DHCP handshakes, all of it.
 * A retry must cost nothing while it waits. */
static esp_timer_handle_t s_retry;
static uint32_t s_retry_ms = 3000;

static void retry_cb(void *arg)
{
    (void)arg;
    if (s_sta_on) esp_wifi_connect();
}

static void schedule_retry(void)
{
    if (!s_retry) {
        const esp_timer_create_args_t a = { .callback = retry_cb, .name = "wifi_retry" };
        if (esp_timer_create(&a, &s_retry) != ESP_OK) return;
    }
    esp_timer_stop(s_retry);                            /* never queue two */
    esp_timer_start_once(s_retry, (uint64_t)s_retry_ms * 1000);
    /* Back off towards half a minute: a bad passphrase should not keep the
     * radio busy competing with the console's own access point. */
    if (s_retry_ms < 30000) s_retry_ms *= 2;
}

bool net_wifi_start_ap(const char *ssid, const char *pass)
{
    if (!pass || strlen(pass) < 8) { ESP_LOGE(TAG, "WPA2 needs a >=8 char passphrase; AP not started"); return false; }
    if (s_active) return true;                       /* already up */

    /* Every step below is checked and RETURNED on, never ESP_ERROR_CHECK'd.
     * ESP_ERROR_CHECK aborts, and an abort here reboots the device - which on
     * a radio that fails to start turns a degraded console into a boot loop.
     * The console is a convenience; the device must still come up without it. */
#define TRY(expr) do { esp_err_t _e = (expr); if (_e != ESP_OK) { \
        ESP_LOGE(TAG, "%s failed: %s", #expr, esp_err_to_name(_e)); return false; } } while (0)

    /* ONE-TIME setup, kept behind a flag.
     *
     * esp_netif_init(), the default event loop, the AP netif and esp_wifi_init()
     * can each only be done once for the life of the process. Running them again
     * on a restart created a second netif and returned an error from
     * esp_wifi_init, so the AP came back up exactly never - which is what
     * happened to anyone who switched the Wi-Fi console off and on again. */
    static bool inited;
    if (!inited) {
        TRY(esp_netif_init());
        esp_err_t ev = esp_event_loop_create_default();
        if (ev != ESP_OK && ev != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "event loop failed: %s", esp_err_to_name(ev)); return false;
        }
        if (!esp_netif_create_default_wifi_ap()) { ESP_LOGE(TAG, "netif create failed"); return false; }
        wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
        TRY(esp_wifi_init(&ic));
        inited = true;
    }

    if (ssid && *ssid) { strncpy(s_ssid, ssid, sizeof(s_ssid) - 1); s_ssid[sizeof(s_ssid)-1] = 0; }
    else {
        uint8_t m[6]; esp_read_mac(m, ESP_MAC_WIFI_SOFTAP);
        snprintf(s_ssid, sizeof(s_ssid), "Dolos-%02X%02X", m[4], m[5]);
    }

    wifi_config_t wc = { 0 };
    /* A 32-character SSID is legal and fills the field exactly - no terminator.
     * strncpy(..., sizeof-1) dropped the last character while ssid_len still
     * claimed all 32, so the beacon advertised a byte that was never set. */
    size_t sl = strlen(s_ssid);
    if (sl > sizeof(wc.ap.ssid)) sl = sizeof(wc.ap.ssid);
    memcpy(wc.ap.ssid, s_ssid, sl);
    wc.ap.ssid_len = (uint8_t)sl;
    strncpy((char *)wc.ap.password, pass, sizeof(wc.ap.password) - 1);
    wc.ap.max_connection = 2;
    wc.ap.channel = 1;
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wc.ap.pmf_cfg.required = false;

    /* Preserve a station connection if one is already running: setting plain
     * AP mode here would silently drop the uplink. */
    wifi_mode_t want = s_sta_on ? WIFI_MODE_APSTA : WIFI_MODE_AP;
    TRY(esp_wifi_set_mode(want));
    TRY(esp_wifi_set_config(WIFI_IF_AP, &wc));
    TRY(esp_wifi_start());
#undef TRY
    s_active = true;
    ESP_LOGI(TAG, "SoftAP '%s' up (WPA2), console at http://192.168.4.1", s_ssid);
    return true;
}

void net_wifi_stop_ap(void)
{
    if (!s_active) return;
    /* esp_wifi_stop() stops the STATION too. Leaving s_sta_on set made the
     * screen and the console both report an uplink that no longer exists. */
    if (s_sta_on) { s_sta_on = false; s_sta_conn = false; s_sta_ip[0] = 0; }
    if (s_retry) esp_timer_stop(s_retry);
    esp_wifi_stop();
    s_active = false;
    ESP_LOGW(TAG, "SoftAP stopped");
}


static void sta_events(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_conn = false;
        s_sta_ip[0] = 0;
        /* Keep trying, but never spin: a wrong password would otherwise pin the
         * CPU and starve the access point the operator is actually using. */
        if (s_sta_on) schedule_retry();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&e->ip_info.ip));
        s_sta_conn = true;
        s_retry_ms = 3000;                              /* a good join resets it */
        ESP_LOGI(TAG, "upstream connected: %s (%s)", s_sta_ssid, s_sta_ip);
    }
}

bool net_wifi_sta_start(const char *ssid, const char *pass)
{
    if (!ssid || !*ssid) return false;
    if (!s_active) { ESP_LOGE(TAG, "the access point must be up first"); return false; }

    static bool handlers;
    if (!handlers) {
        if (esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                sta_events, NULL, NULL) != ESP_OK ||
            esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                sta_events, NULL, NULL) != ESP_OK) {
            ESP_LOGE(TAG, "could not register station events");
            return false;
        }
        handlers = true;
    }
    static esp_netif_t *sta_netif;
    if (!sta_netif) sta_netif = esp_netif_create_default_wifi_sta();
    if (!sta_netif) { ESP_LOGE(TAG, "no station interface"); return false; }

    wifi_config_t wc = { 0 };
    size_t sl = strlen(ssid);                     /* same 32-character case */
    if (sl > sizeof(wc.sta.ssid)) sl = sizeof(wc.sta.ssid);
    memcpy(wc.sta.ssid, ssid, sl);
    if (pass) snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", pass);
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;

    esp_err_t e;
    if ((e = esp_wifi_set_mode(WIFI_MODE_APSTA)) != ESP_OK ||
        (e = esp_wifi_set_config(WIFI_IF_STA, &wc)) != ESP_OK) {
        ESP_LOGE(TAG, "station config failed: %s", esp_err_to_name(e));
        esp_wifi_set_mode(WIFI_MODE_AP);
        return false;
    }
    snprintf(s_sta_ssid, sizeof(s_sta_ssid), "%s", ssid);
    s_sta_on = true; s_sta_conn = false; s_sta_ip[0] = 0;
    esp_wifi_connect();
    ESP_LOGW(TAG, "joining '%s' - this device is now reachable from that network", ssid);
    return true;
}

void net_wifi_sta_stop(void)
{
    if (!s_sta_on) return;
    s_sta_on = false; s_sta_conn = false; s_sta_ip[0] = 0; s_sta_ssid[0] = 0;
    if (s_retry) esp_timer_stop(s_retry);          /* no join after we stopped */
    esp_wifi_disconnect();
    esp_wifi_set_mode(WIFI_MODE_AP);      /* back to console-only */
    ESP_LOGI(TAG, "upstream disconnected");
}

bool net_wifi_sta_enabled(void)   { return s_sta_on; }
bool net_wifi_sta_connected(void) { return s_sta_conn; }
const char *net_wifi_sta_ssid(void) { return s_sta_ssid; }
const char *net_wifi_sta_ip(void)   { return s_sta_ip; }

const char *net_wifi_ssid(void) { return s_ssid; }
bool net_wifi_active(void) { return s_active; }
