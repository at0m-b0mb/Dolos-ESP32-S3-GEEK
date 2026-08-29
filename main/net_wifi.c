#include "net_wifi.h"
#include <string.h>
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "esp_log.h"

static const char *TAG = "net_wifi";
static char s_ssid[33];
static bool s_active;

bool net_wifi_start_ap(const char *ssid, const char *pass)
{
    if (!pass || strlen(pass) < 8) { ESP_LOGE(TAG, "WPA2 needs a >=8 char passphrase; AP not started"); return false; }

    /* Every step below is checked and RETURNED on, never ESP_ERROR_CHECK'd.
     * ESP_ERROR_CHECK aborts, and an abort here reboots the device - which on
     * a radio that fails to start (out of memory, bad config) turns a degraded
     * console into an endless boot loop. The console is a convenience; the
     * device must still come up without it. */
#define TRY(expr) do { esp_err_t _e = (expr); if (_e != ESP_OK) { \
        ESP_LOGE(TAG, "%s failed: %s", #expr, esp_err_to_name(_e)); return false; } } while (0)

    TRY(esp_netif_init());
    esp_err_t ev = esp_event_loop_create_default();
    if (ev != ESP_OK && ev != ESP_ERR_INVALID_STATE) {   /* already created is fine */
        ESP_LOGE(TAG, "event loop failed: %s", esp_err_to_name(ev)); return false;
    }
    if (!esp_netif_create_default_wifi_ap()) { ESP_LOGE(TAG, "netif create failed"); return false; }
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    TRY(esp_wifi_init(&ic));

    if (ssid && *ssid) { strncpy(s_ssid, ssid, sizeof(s_ssid) - 1); }
    else {
        uint8_t m[6]; esp_read_mac(m, ESP_MAC_WIFI_SOFTAP);
        snprintf(s_ssid, sizeof(s_ssid), "Dolos-%02X%02X", m[4], m[5]);
    }

    wifi_config_t wc = { 0 };
    strncpy((char *)wc.ap.ssid, s_ssid, sizeof(wc.ap.ssid) - 1);
    wc.ap.ssid_len = strlen(s_ssid);
    strncpy((char *)wc.ap.password, pass, sizeof(wc.ap.password) - 1);
    wc.ap.max_connection = 2;
    wc.ap.channel = 1;
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wc.ap.pmf_cfg.required = false;

    TRY(esp_wifi_set_mode(WIFI_MODE_AP));
    TRY(esp_wifi_set_config(WIFI_IF_AP, &wc));
    TRY(esp_wifi_start());
#undef TRY
    s_active = true;
    ESP_LOGI(TAG, "SoftAP '%s' up (WPA2), console at http://192.168.4.1", s_ssid);
    return true;
}

const char *net_wifi_ssid(void) { return s_ssid; }
bool net_wifi_active(void) { return s_active; }
