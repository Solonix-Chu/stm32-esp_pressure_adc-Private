#include "wifi_ap.h"

#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"

static const char *TAG = "wifi_ap";
static bool s_started;

esp_err_t wifi_ap_start(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    wifi_config_t wifi_config = {
        .ap =
            {
                .channel = CONFIG_ADC_LOGGER_WIFI_AP_CHANNEL,
                .max_connection = 4,
            },
    };

    if (s_started) {
        return ESP_OK;
    }

    esp_err_t err = esp_netif_init();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    err = esp_event_loop_create_default();
    if ((err != ESP_OK) && (err != ESP_ERR_INVALID_STATE)) {
        return err;
    }

    (void) esp_netif_create_default_wifi_ap();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");

    strlcpy((char *) wifi_config.ap.ssid,
            CONFIG_ADC_LOGGER_WIFI_AP_SSID,
            sizeof(wifi_config.ap.ssid));
    strlcpy((char *) wifi_config.ap.password,
            CONFIG_ADC_LOGGER_WIFI_AP_PASSWORD,
            sizeof(wifi_config.ap.password));
    wifi_config.ap.ssid_len = strlen(CONFIG_ADC_LOGGER_WIFI_AP_SSID);
    wifi_config.ap.authmode =
        (strlen(CONFIG_ADC_LOGGER_WIFI_AP_PASSWORD) >= 8U) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "wifi mode set failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wifi_config), TAG, "wifi config set failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");

    s_started = true;
    ESP_LOGI(TAG,
             "softAP started: ssid=%s channel=%d auth=%s ip=%s",
             CONFIG_ADC_LOGGER_WIFI_AP_SSID,
             CONFIG_ADC_LOGGER_WIFI_AP_CHANNEL,
             (wifi_config.ap.authmode == WIFI_AUTH_OPEN) ? "open" : "wpa2",
             wifi_ap_get_ip());
    return ESP_OK;
}

const char *wifi_ap_get_ssid(void)
{
    return CONFIG_ADC_LOGGER_WIFI_AP_SSID;
}

const char *wifi_ap_get_ip(void)
{
    return "192.168.4.1";
}
