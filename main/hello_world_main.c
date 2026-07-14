#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "display.h"
#include "wifi.h"
#include "server.h"
#include "config.h"
#include "stocks.h"
#include "game.h"
#include "mdns.h"

/* Used only on first boot; afterwards the config comes from NVS
 * and can be edited on the web dashboard. These values define the
 * ESP32-hosted Wi-Fi network users join to reach the dashboard. */
#define DEFAULT_WIFI_SSID     "Maddox-OS"
#define DEFAULT_WIFI_PASSWORD "maddox-os"
#define DEFAULT_BOOT_MESSAGE  "Hello Maddox"
/* SpaceX, public on Nasdaq since its June 2026 IPO */
#define DEFAULT_STOCK_SYMBOL  "SPCX"
#define PLAYER_URL            "http://192.168.4.1/player"
#define HOST_URL              "http://192.168.4.1/host"

static const char *TAG = "maddox-os";

/* Config must outlive app_main: the server keeps a pointer to it */
static device_config_t s_config;

/* QR code for PLAYER_URL, version 2-L, 25x25 modules. */
static const char *DASHBOARD_QR[] = {
    "1111111011100111101111111",
    "1000001010000010001000001",
    "1011101000111100101011101",
    "1011101011000110001011101",
    "1011101011011001001011101",
    "1000001000001010101000001",
    "1111111010101010101111111",
    "0000000000001011000000000",
    "1000001100011010010011011",
    "1010010110101101000100001",
    "1001101001001111100010111",
    "1111110000000000110110111",
    "0100101110100101101011111",
    "1010010010101101000110100",
    "1011111111001010110110011",
    "0010100101000000001100000",
    "0011101010010100111110101",
    "0000000001001101100010010",
    "1111111011000011101010111",
    "1000001001011110100011110",
    "1011101001001111111110011",
    "1011101001110010101101011",
    "1011101001010011111000100",
    "1000001001010110110010110",
    "1111111010111011001111111",
};

static void show_boot_message(const char *text)
{
    const int chars_per_line = DISPLAY_WIDTH / 6;

    display_clear();
    char line[chars_per_line + 1];
    const char *p = text;
    for (int page = 0; page < DISPLAY_PAGES && *p != '\0'; page++) {
        int n = strnlen(p, chars_per_line);
        memcpy(line, p, n);
        line[n] = '\0';
        display_draw_text(0, page, line);
        p += n;
    }
    display_flush();
}

static void show_dashboard_qr(void)
{
    display_lock();
    display_clear();

    const int quiet = 4;
    const int origin_x = 1;
    const int origin_y = 1;
    for (int y = 0; y < 25; y++) {
        for (int x = 0; x < 25; x++) {
            if (DASHBOARD_QR[y][x] == '1') {
                display_draw_pixel(origin_x + quiet + x, origin_y + quiet + y, true);
            }
        }
    }

    display_draw_text(36, 0, "JOIN");
    display_draw_text(36, 1, "WIFI");
    display_draw_text(36, 2, "SCAN");
    display_draw_text(0, 4, "192.168.4.1");
    display_flush();
    display_unlock();
}

void app_main(void)
{
    esp_err_t err = display_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display init failed: %s", esp_err_to_name(err));
        return;
    }

    const device_config_t defaults = {
        .wifi_ssid     = DEFAULT_WIFI_SSID,
        .wifi_password = DEFAULT_WIFI_PASSWORD,
        .boot_message  = DEFAULT_BOOT_MESSAGE,
        .stock_symbol  = DEFAULT_STOCK_SYMBOL,
    };
    err = config_load(&s_config, &defaults);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config load failed: %s", esp_err_to_name(err));
        return;
    }

    show_boot_message(s_config.boot_message);
    ESP_LOGI(TAG, "Boot message: %s", s_config.boot_message);

    err = wifi_init_ap(s_config.wifi_ssid, s_config.wifi_password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi access point failed: %s", esp_err_to_name(err));
        return;
    }

    show_dashboard_qr();

    /* Advertise the device as maddox-os.local on the AP network */
    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set("maddox-os");
    mdns_instance_name_set("maddox-os dashboard");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    game_init();
    ESP_LOGI(TAG, "Players join Wi-Fi '%s' and open %s", s_config.wifi_ssid, PLAYER_URL);
    ESP_LOGI(TAG, "Host opens %s", HOST_URL);

    err = server_start(&s_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "server start failed: %s", esp_err_to_name(err));
        return;
    }

    err = stocks_start(&s_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "stocks start failed: %s", esp_err_to_name(err));
        return;
    }
    stocks_set_mode(false);

    ESP_LOGI(TAG, "Web server is up");
}
