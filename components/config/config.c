#include "config.h"

#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "CONFIG";

#define NVS_NAMESPACE "maddox"

static esp_err_t ensure_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static void load_str(nvs_handle_t handle, const char *key,
                     char *dst, size_t dst_size, const char *fallback)
{
    size_t len = dst_size;
    if (nvs_get_str(handle, key, dst, &len) != ESP_OK) {
        strlcpy(dst, fallback, dst_size);
    }
}

esp_err_t config_load(device_config_t *cfg, const device_config_t *defaults)
{
    esp_err_t err = ensure_nvs();
    if (err != ESP_OK) {
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        /* Namespace doesn't exist yet (first boot): use defaults */
        *cfg = *defaults;
        ESP_LOGI(TAG, "No stored config, using defaults");
        return ESP_OK;
    }

    load_str(handle, "wifi_ssid", cfg->wifi_ssid, sizeof(cfg->wifi_ssid),
             defaults->wifi_ssid);
    load_str(handle, "wifi_pass", cfg->wifi_password, sizeof(cfg->wifi_password),
             defaults->wifi_password);
    load_str(handle, "boot_msg", cfg->boot_message, sizeof(cfg->boot_message),
             defaults->boot_message);
    load_str(handle, "symbol", cfg->stock_symbol, sizeof(cfg->stock_symbol),
             defaults->stock_symbol);

    nvs_close(handle);
    ESP_LOGI(TAG, "Config loaded (ssid=%s)", cfg->wifi_ssid);
    return ESP_OK;
}

esp_err_t config_save(const device_config_t *cfg)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, "wifi_ssid", cfg->wifi_ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "wifi_pass", cfg->wifi_password);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "boot_msg", cfg->boot_message);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(handle, "symbol", cfg->stock_symbol);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Config saved");
    } else {
        ESP_LOGE(TAG, "Config save failed: %s", esp_err_to_name(err));
    }
    return err;
}
