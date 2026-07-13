#pragma once

#include "esp_err.h"

esp_err_t wifi_init_sta(const char *ssid, const char *password);
esp_err_t wifi_init_ap(const char *ssid, const char *password);
