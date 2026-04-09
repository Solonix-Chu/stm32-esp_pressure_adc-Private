#include "adc_logger.h"
#include "web_server.h"
#include "wifi_ap.h"

#include "esp_err.h"
#include "esp_log.h"
#include "nvs_flash.h"

static const char *TAG = "app";

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if ((err == ESP_ERR_NVS_NO_FREE_PAGES) || (err == ESP_ERR_NVS_NEW_VERSION_FOUND)) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

void app_main(void)
{
    init_nvs();

    ESP_ERROR_CHECK(adc_logger_start());
    ESP_ERROR_CHECK(wifi_ap_start());
    ESP_ERROR_CHECK(web_server_start());

    ESP_LOGI(TAG,
             "ESP32 ADC logger ready. Connect to Wi-Fi SSID '%s' and open http://%s",
             wifi_ap_get_ssid(),
             wifi_ap_get_ip());
}
