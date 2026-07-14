#pragma once

#include "esp_err.h"
#include "config.h"

/* Starts the HTTP web server on port 80.
 * Wi-Fi must be connected before calling this.
 * `cfg` must stay valid for the lifetime of the server;
 * the config endpoints read from and write to it. */
esp_err_t server_start(device_config_t *cfg);

/* Stops the web server if it is running. */
esp_err_t server_stop(void);
