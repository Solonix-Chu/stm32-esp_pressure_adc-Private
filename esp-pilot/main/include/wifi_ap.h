#ifndef WIFI_AP_H
#define WIFI_AP_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wifi_ap_start(void);
const char *wifi_ap_get_ssid(void);
const char *wifi_ap_get_ip(void);

#ifdef __cplusplus
}
#endif

#endif
