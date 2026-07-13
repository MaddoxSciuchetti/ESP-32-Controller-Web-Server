#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "config.h"

typedef struct {
    char symbol[CONFIG_SYMBOL_MAX + 1];
    double price;
    double change_pct;
    bool valid;
    char error[48];
} stock_quote_t;

/* Starts the background task that polls the quote API and renders
 * the result on the OLED. Reads the symbol from `cfg`, which must
 * stay valid forever. Call after Wi-Fi is connected. */
esp_err_t stocks_start(device_config_t *cfg);

/* Copies the latest quote into `out` (thread-safe). */
void stocks_get_quote(stock_quote_t *out);

/* Fetch again as soon as possible (e.g. after the symbol changed). */
void stocks_refresh_now(void);

/* Keep the stock screen off the OLED for `seconds`, so a
 * user-posted message stays visible. */
void stocks_hold_screen(int seconds);

/* Screen mode: true = stock quote owns the OLED,
 * false = a user message owns it and the quote never draws. */
void stocks_set_mode(bool show_stock);
bool stocks_get_mode(void);
