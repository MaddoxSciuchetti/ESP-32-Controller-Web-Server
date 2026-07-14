#include "stocks.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "display.h"

static const char *TAG = "STOCKS";

#define FETCH_INTERVAL_US (3LL * 60 * 1000000) /* every 3 minutes */
#define RENDER_TICK_MS    5000
#define RESPONSE_BUF_SIZE 8192

static device_config_t *s_config;
static stock_quote_t s_quote;
static SemaphoreHandle_t s_quote_mutex;
static SemaphoreHandle_t s_nudge;
static volatile int64_t s_hold_until_us;
static volatile bool s_stock_mode = true;

static bool symbol_is_valid(const char *s)
{
    size_t len = strlen(s);
    if (len == 0 || len > CONFIG_SYMBOL_MAX) {
        return false;
    }
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && *s != '.' && *s != '-' && *s != '^') {
            return false;
        }
    }
    return true;
}

static void set_error(const char *symbol, const char *msg)
{
    xSemaphoreTake(s_quote_mutex, portMAX_DELAY);
    strlcpy(s_quote.symbol, symbol, sizeof(s_quote.symbol));
    s_quote.valid = false;
    strlcpy(s_quote.error, msg, sizeof(s_quote.error));
    xSemaphoreGive(s_quote_mutex);
    ESP_LOGW(TAG, "%s: %s", symbol, msg);
}

static void parse_response(const char *symbol, const char *body)
{
    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        set_error(symbol, "bad JSON from API");
        return;
    }

    const cJSON *chart = cJSON_GetObjectItem(root, "chart");
    const cJSON *api_error = cJSON_GetObjectItem(chart, "error");
    if (cJSON_IsObject(api_error)) {
        set_error(symbol, "symbol not found");
        cJSON_Delete(root);
        return;
    }

    const cJSON *result = cJSON_GetArrayItem(cJSON_GetObjectItem(chart, "result"), 0);
    const cJSON *meta = cJSON_GetObjectItem(result, "meta");
    const cJSON *price = cJSON_GetObjectItem(meta, "regularMarketPrice");
    const cJSON *prev = cJSON_GetObjectItem(meta, "chartPreviousClose");
    if (!cJSON_IsNumber(prev)) {
        prev = cJSON_GetObjectItem(meta, "previousClose");
    }

    if (!cJSON_IsNumber(price)) {
        set_error(symbol, "no price in response");
        cJSON_Delete(root);
        return;
    }

    double p = price->valuedouble;
    double pct = 0;
    if (cJSON_IsNumber(prev) && prev->valuedouble != 0) {
        pct = (p - prev->valuedouble) / prev->valuedouble * 100.0;
    }

    xSemaphoreTake(s_quote_mutex, portMAX_DELAY);
    strlcpy(s_quote.symbol, symbol, sizeof(s_quote.symbol));
    s_quote.price = p;
    s_quote.change_pct = pct;
    s_quote.valid = true;
    s_quote.error[0] = '\0';
    xSemaphoreGive(s_quote_mutex);

    ESP_LOGI(TAG, "%s: %.2f (%+.2f%%)", symbol, p, pct);
    cJSON_Delete(root);
}

static void fetch_quote(void)
{
    char symbol[CONFIG_SYMBOL_MAX + 1];
    strlcpy(symbol, s_config->stock_symbol, sizeof(symbol));

    if (!symbol_is_valid(symbol)) {
        set_error(symbol, "invalid symbol");
        return;
    }

    char url[128];
    snprintf(url, sizeof(url),
             "https://query1.finance.yahoo.com/v8/finance/chart/%s"
             "?interval=1d&range=1d", symbol);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        set_error(symbol, "http client init failed");
        return;
    }
    /* Yahoo rejects requests without a browser-like User-Agent */
    esp_http_client_set_header(client, "User-Agent", "Mozilla/5.0 (maddox-os)");
    esp_http_client_set_header(client, "Accept", "application/json");

    char *buf = malloc(RESPONSE_BUF_SIZE);
    if (buf == NULL) {
        set_error(symbol, "out of memory");
        esp_http_client_cleanup(client);
        return;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        set_error(symbol, "connection failed");
        goto out;
    }

    esp_http_client_fetch_headers(client);
    int total = 0;
    while (total < RESPONSE_BUF_SIZE - 1) {
        int n = esp_http_client_read(client, buf + total,
                                     RESPONSE_BUF_SIZE - 1 - total);
        if (n <= 0) {
            break;
        }
        total += n;
    }
    buf[total] = '\0';

    int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "GET %s -> %d (%d bytes)", symbol, status, total);

    /* Parse even on non-200: Yahoo returns a JSON error body for
     * unknown symbols, which gives a better message than the code */
    parse_response(symbol, buf);

out:
    free(buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
}

static void render_quote(void)
{
    stock_quote_t q;
    stocks_get_quote(&q);
    if (!q.valid) {
        return;
    }

    char price_line[16];
    char change_line[16];
    snprintf(price_line, sizeof(price_line), "%.2f", q.price);
    snprintf(change_line, sizeof(change_line), "%+.2f%%", q.change_pct);

    display_lock();
    display_clear();
    display_draw_text(0, 0, q.symbol);
    display_draw_text(0, 2, price_line);
    display_draw_text(0, 4, change_line);
    display_flush();
    display_unlock();
}

static void stocks_task(void *arg)
{
    int64_t last_fetch = -FETCH_INTERVAL_US;

    for (;;) {
        if (esp_timer_get_time() - last_fetch >= FETCH_INTERVAL_US) {
            fetch_quote();
            last_fetch = esp_timer_get_time();
        }

        if (s_stock_mode && esp_timer_get_time() >= s_hold_until_us) {
            render_quote();
        }

        /* Sleep, but wake early if someone requests a refresh */
        if (xSemaphoreTake(s_nudge, pdMS_TO_TICKS(RENDER_TICK_MS)) == pdTRUE) {
            last_fetch = -FETCH_INTERVAL_US;
        }
    }
}

esp_err_t stocks_start(device_config_t *cfg)
{
    s_config = cfg;
    s_quote_mutex = xSemaphoreCreateMutex();
    s_nudge = xSemaphoreCreateBinary();

    BaseType_t ok = xTaskCreate(stocks_task, "stocks", 12288, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void stocks_get_quote(stock_quote_t *out)
{
    xSemaphoreTake(s_quote_mutex, portMAX_DELAY);
    *out = s_quote;
    xSemaphoreGive(s_quote_mutex);
}

void stocks_refresh_now(void)
{
    xSemaphoreGive(s_nudge);
}

void stocks_hold_screen(int seconds)
{
    s_hold_until_us = esp_timer_get_time() + (int64_t)seconds * 1000000;
}

void stocks_set_mode(bool show_stock)
{
    s_stock_mode = show_stock;
    if (show_stock) {
        /* Retake the screen right away with a fresh quote */
        s_hold_until_us = 0;
        xSemaphoreGive(s_nudge);
    }
}

bool stocks_get_mode(void)
{
    return s_stock_mode;
}
