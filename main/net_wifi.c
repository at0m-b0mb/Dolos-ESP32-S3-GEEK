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

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));

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

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_active = true;
    ESP_LOGI(TAG, "SoftAP '%s' up (WPA2), console at http://192.168.4.1", s_ssid);
    return true;
}

const char *net_wifi_ssid(void) { return s_ssid; }
bool net_wifi_active(void) { return s_active; }
