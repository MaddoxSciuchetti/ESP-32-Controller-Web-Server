#pragma once

#include "esp_err.h"

#define CONFIG_SSID_MAX   32
#define CONFIG_PASS_MAX   64
#define CONFIG_MSG_MAX    60
#define CONFIG_SYMBOL_MAX 7

typedef struct {
    char wifi_ssid[CONFIG_SSID_MAX + 1];
    char wifi_password[CONFIG_PASS_MAX + 1];
    char boot_message[CONFIG_MSG_MAX + 1];
    char stock_symbol[CONFIG_SYMBOL_MAX + 1];
} device_config_t;

/* Loads config from NVS. Any key not yet stored falls back to the
 * value in `defaults`. Also initializes NVS on first call. */
esp_err_t config_load(device_config_t *cfg, const device_config_t *defaults);

/* Persists the whole config to NVS. Takes effect on next boot. */
esp_err_t config_save(const device_config_t *cfg);
