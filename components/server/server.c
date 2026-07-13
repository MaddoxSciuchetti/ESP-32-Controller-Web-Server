#include "server.h"

#include <stdbool.h>
#include <string.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_http_server.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "display.h"
#include "game.h"
#include "stocks.h"

static const char *TAG = "SERVER";

/* 6 px per character on a 72 px wide panel */
#define CHARS_PER_LINE (DISPLAY_WIDTH / 6)

static httpd_handle_t s_server = NULL;
static device_config_t *s_config = NULL;
static char s_oled_message[CONFIG_MSG_MAX + 1] = "";

/* HTML files embedded into the binary via EMBED_TXTFILES */
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char host_html_start[] asm("_binary_host_html_start");
extern const char player_html_start[] asm("_binary_player_html_start");

static const char *reset_reason_str(void)
{
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:   return "power-on";
        case ESP_RST_SW:        return "software reboot";
        case ESP_RST_PANIC:     return "crash (panic)";
        case ESP_RST_WDT:
        case ESP_RST_INT_WDT:
        case ESP_RST_TASK_WDT:  return "watchdog";
        case ESP_RST_BROWNOUT:  return "brownout";
        case ESP_RST_DEEPSLEEP: return "deep sleep wake";
        default:                return "other";
    }
}

static esp_err_t draw_message(const char *text)
{
    display_lock();
    display_clear();
    char line[CHARS_PER_LINE + 1];
    const char *p = text;
    for (int page = 0; page < DISPLAY_PAGES && *p != '\0'; page++) {
        int n = strnlen(p, CHARS_PER_LINE);
        memcpy(line, p, n);
        line[n] = '\0';
        display_draw_text(0, page, line);
        p += n;
    }
    esp_err_t err = display_flush();
    display_unlock();
    return err;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t host_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, host_html_start, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t player_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, player_html_start, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t send_json(httpd_req_t *req, cJSON *json)
{
    char *body = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (body == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "JSON allocation failed");
    }

    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, body);
    cJSON_free(body);
    return err;
}

static cJSON *game_snapshot_json(void)
{
    game_snapshot_t snap;
    game_get_snapshot(&snap);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "phase", game_phase_name(snap.phase));
    cJSON_AddNumberToObject(root, "session_id", snap.session_id);
    cJSON_AddNumberToObject(root, "question_index", snap.question_index);
    cJSON_AddNumberToObject(root, "question_count", snap.question_count);
    cJSON_AddNumberToObject(root, "total_votes", snap.total_votes);
    cJSON_AddNumberToObject(root, "unique_clients", snap.unique_clients);

    if (snap.question_index >= 0 && snap.phase != GAME_PHASE_DONE) {
        cJSON_AddStringToObject(root, "question", snap.question.question);
    } else {
        cJSON_AddStringToObject(root, "question", "");
    }

    cJSON *options = cJSON_AddArrayToObject(root, "options");
    cJSON *votes = cJSON_AddArrayToObject(root, "votes");
    for (int i = 0; i < snap.question.option_count; i++) {
        cJSON_AddItemToArray(options, cJSON_CreateString(snap.question.options[i]));
        cJSON_AddItemToArray(votes, cJSON_CreateNumber(snap.votes[i]));
    }

    cJSON_AddNumberToObject(root, "correct_index",
                            snap.phase == GAME_PHASE_REVEALED ? snap.question.correct_index : -1);
    return root;
}

static esp_err_t draw_game_score(void)
{
    game_snapshot_t snap;
    game_get_snapshot(&snap);

    char line0[32];
    char line1[32];
    char line2[32];
    char line3[32];
    char line4[32];

    if (snap.phase == GAME_PHASE_IDLE) {
        snprintf(line0, sizeof(line0), "READY");
        snprintf(line1, sizeof(line1), "HOST START");
        snprintf(line2, sizeof(line2), "PLAYERS");
        snprintf(line3, sizeof(line3), "JOIN WIFI");
        snprintf(line4, sizeof(line4), "192.168.4.1");
    } else if (snap.phase == GAME_PHASE_DONE) {
        snprintf(line0, sizeof(line0), "GAME DONE");
        snprintf(line1, sizeof(line1), "FINAL");
        snprintf(line2, sizeof(line2), "VOTES %d", snap.total_votes);
        snprintf(line3, sizeof(line3), "OPEN HOST");
        snprintf(line4, sizeof(line4), "192.168.4.1");
    } else {
        const int correct = snap.question.correct_index;
        const int correct_votes =
            (correct >= 0 && correct < GAME_MAX_OPTIONS) ? snap.votes[correct] : 0;

        snprintf(line0, sizeof(line0), "Q %d/%d",
                 snap.question_index + 1, snap.question_count);
        snprintf(line1, sizeof(line1), "%s", snap.phase == GAME_PHASE_OPEN ? "LIVE SCORE" : "REVEALED");
        snprintf(line2, sizeof(line2), "CORRECT");
        snprintf(line3, sizeof(line3), "%d/%d", correct_votes, snap.total_votes);
        snprintf(line4, sizeof(line4), "PLAYERS %d", snap.unique_clients);
    }

    display_lock();
    display_clear();
    display_draw_text(0, 0, line0);
    display_draw_text(0, 1, line1);
    display_draw_text(0, 2, line2);
    display_draw_text(0, 3, line3);
    display_draw_text(0, 4, line4);
    esp_err_t err = display_flush();
    display_unlock();
    return err;
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    esp_netif_ip_info_t ip_info = {0};
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (netif != NULL) {
        esp_netif_get_ip_info(netif, &ip_info);
    }

    esp_chip_info_t chip;
    esp_chip_info(&chip);

    const esp_app_desc_t *app = esp_app_get_description();

    stock_quote_t quote;
    stocks_get_quote(&quote);

    char buf[768];
    snprintf(buf, sizeof(buf),
             "{"
             "\"uptime\":%lld,"
             "\"free_heap\":%lu,"
             "\"min_free_heap\":%lu,"
             "\"rssi\":%d,"
             "\"ssid\":\"%s\","
             "\"ip\":\"" IPSTR "\","
             "\"chip\":\"ESP32-C3 rev %d.%d\","
             "\"idf_version\":\"%s\","
             "\"compile_time\":\"%s %s\","
             "\"reset_reason\":\"%s\","
             "\"oled_message\":\"%s\","
             "\"stock_symbol\":\"%s\","
             "\"stock_price\":%.2f,"
             "\"stock_change_pct\":%.2f,"
             "\"stock_error\":\"%s\","
             "\"display_mode\":\"%s\""
             "}",
             esp_timer_get_time() / 1000000,
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)esp_get_minimum_free_heap_size(),
             0,
             s_config->wifi_ssid,
             IP2STR(&ip_info.ip),
             chip.revision / 100, chip.revision % 100,
             IDF_VER,
             app->date, app->time,
             reset_reason_str(),
             s_oled_message,
             quote.symbol,
             quote.valid ? quote.price : 0.0,
             quote.valid ? quote.change_pct : 0.0,
             quote.valid ? "" : quote.error,
             stocks_get_mode() ? "stock" : "message");

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t display_post_handler(httpd_req_t *req)
{
    /* 5 pages x 12 chars = 60 chars fit on the panel */
    char buf[CHARS_PER_LINE * DISPLAY_PAGES + 1];

    if (req->content_len >= sizeof(buf)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Message too long (max 60 chars)");
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret <= 0) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "Failed to read request body");
        }
        received += ret;
    }
    buf[received] = '\0';

    ESP_LOGI(TAG, "Displaying: \"%s\"", buf);

    esp_err_t err = draw_message(buf);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Display update failed");
    }

    strlcpy(s_oled_message, buf, sizeof(s_oled_message));
    /* In stock mode the message borrows the screen for 30 s;
     * in message mode it stays until the mode is toggled back */
    stocks_hold_screen(30);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"displayed\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t config_get_handler(httpd_req_t *req)
{
    /* Never send the Wi-Fi password back to the browser */
    char buf[224];
    snprintf(buf, sizeof(buf),
             "{\"wifi_ssid\":\"%s\",\"boot_message\":\"%s\",\"stock_symbol\":\"%s\"}",
             s_config->wifi_ssid, s_config->boot_message, s_config->stock_symbol);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t config_post_handler(httpd_req_t *req)
{
    char buf[256];
    if (req->content_len >= sizeof(buf)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret <= 0) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "Failed to read request body");
        }
        received += ret;
    }
    buf[received] = '\0';

    cJSON *json = cJSON_Parse(buf);
    if (json == NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    const cJSON *ssid = cJSON_GetObjectItem(json, "wifi_ssid");
    const cJSON *pass = cJSON_GetObjectItem(json, "wifi_password");
    const cJSON *msg  = cJSON_GetObjectItem(json, "boot_message");
    const cJSON *sym  = cJSON_GetObjectItem(json, "stock_symbol");

    if (cJSON_IsString(ssid) && ssid->valuestring[0] != '\0') {
        strlcpy(s_config->wifi_ssid, ssid->valuestring, sizeof(s_config->wifi_ssid));
    }
    if (cJSON_IsString(pass) && pass->valuestring[0] != '\0') {
        strlcpy(s_config->wifi_password, pass->valuestring,
                sizeof(s_config->wifi_password));
    }
    if (cJSON_IsString(msg)) {
        strlcpy(s_config->boot_message, msg->valuestring,
                sizeof(s_config->boot_message));
    }
    bool symbol_changed = false;
    if (cJSON_IsString(sym) && sym->valuestring[0] != '\0' &&
        strcmp(sym->valuestring, s_config->stock_symbol) != 0) {
        strlcpy(s_config->stock_symbol, sym->valuestring,
                sizeof(s_config->stock_symbol));
        /* Tickers are conventionally uppercase */
        for (char *c = s_config->stock_symbol; *c; c++) {
            if (*c >= 'a' && *c <= 'z') {
                *c -= 'a' - 'A';
            }
        }
        symbol_changed = true;
    }
    cJSON_Delete(json);

    esp_err_t err = config_save(s_config);
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Failed to save config");
    }

    if (symbol_changed) {
        stocks_refresh_now();
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"saved\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t mode_post_handler(httpd_req_t *req)
{
    char buf[16];
    if (req->content_len >= sizeof(buf)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad mode");
    }
    int received = httpd_req_recv(req, buf, req->content_len);
    if (received <= 0) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Failed to read request body");
    }
    buf[received] = '\0';

    if (strcmp(buf, "stock") == 0) {
        stocks_set_mode(true);
    } else if (strcmp(buf, "message") == 0) {
        stocks_set_mode(false);
        /* Show the last message right away, if there is one */
        if (s_oled_message[0] != '\0') {
            draw_message(s_oled_message);
        }
    } else {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Mode must be 'stock' or 'message'");
    }

    ESP_LOGI(TAG, "Display mode: %s", buf);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"ok\":true}", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t game_state_get_handler(httpd_req_t *req)
{
    return send_json(req, game_snapshot_json());
}

static esp_err_t game_start_post_handler(httpd_req_t *req)
{
    esp_err_t err = game_start_next();
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Failed to start question");
    }
    err = draw_game_score();
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Display update failed");
    }
    ESP_LOGI(TAG, "Game start/next");
    return send_json(req, game_snapshot_json());
}

static esp_err_t game_reveal_post_handler(httpd_req_t *req)
{
    esp_err_t err = game_reveal();
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "No open question to reveal");
    }
    err = draw_game_score();
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Display update failed");
    }
    ESP_LOGI(TAG, "Game reveal");
    return send_json(req, game_snapshot_json());
}

static esp_err_t game_reset_post_handler(httpd_req_t *req)
{
    game_reset();
    esp_err_t err = draw_game_score();
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Display update failed");
    }
    ESP_LOGI(TAG, "Game reset");
    return send_json(req, game_snapshot_json());
}

static esp_err_t game_vote_post_handler(httpd_req_t *req)
{
    char buf[128];
    if (req->content_len >= sizeof(buf)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body too large");
    }

    int received = 0;
    while (received < req->content_len) {
        int ret = httpd_req_recv(req, buf + received, req->content_len - received);
        if (ret <= 0) {
            return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                       "Failed to read request body");
        }
        received += ret;
    }
    buf[received] = '\0';

    cJSON *json = cJSON_Parse(buf);
    if (json == NULL) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
    }

    const cJSON *client = cJSON_GetObjectItem(json, "client_id");
    const cJSON *option = cJSON_GetObjectItem(json, "option");
    if (!cJSON_IsString(client) || !cJSON_IsNumber(option)) {
        cJSON_Delete(json);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Expected client_id and option");
    }

    esp_err_t err = game_vote(client->valuestring, option->valueint);
    cJSON_Delete(json);
    if (err == ESP_ERR_INVALID_STATE) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                                   "Voting is not open");
    }
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Vote rejected");
    }

    err = draw_game_score();
    if (err != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                                   "Display update failed");
    }

    return send_json(req, game_snapshot_json());
}

static void reboot_timer_cb(void *arg)
{
    esp_restart();
}

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    ESP_LOGW(TAG, "Reboot requested via web");

    /* Give the HTTP response time to reach the client before restarting */
    const esp_timer_create_args_t args = {
        .callback = reboot_timer_cb,
        .name = "reboot",
    };
    esp_timer_handle_t timer;
    ESP_ERROR_CHECK(esp_timer_create(&args, &timer));
    ESP_ERROR_CHECK(esp_timer_start_once(timer, 500 * 1000));

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"rebooting\":true}", HTTPD_RESP_USE_STRLEN);
}

static const httpd_uri_t root_uri = {
    .uri     = "/",
    .method  = HTTP_GET,
    .handler = root_get_handler,
};

static const httpd_uri_t host_uri = {
    .uri     = "/host",
    .method  = HTTP_GET,
    .handler = host_get_handler,
};

static const httpd_uri_t player_uri = {
    .uri     = "/player",
    .method  = HTTP_GET,
    .handler = player_get_handler,
};

static const httpd_uri_t status_uri = {
    .uri     = "/api/status",
    .method  = HTTP_GET,
    .handler = status_get_handler,
};

static const httpd_uri_t display_uri = {
    .uri     = "/api/display",
    .method  = HTTP_POST,
    .handler = display_post_handler,
};

static const httpd_uri_t config_get_uri = {
    .uri     = "/api/config",
    .method  = HTTP_GET,
    .handler = config_get_handler,
};

static const httpd_uri_t config_post_uri = {
    .uri     = "/api/config",
    .method  = HTTP_POST,
    .handler = config_post_handler,
};

static const httpd_uri_t reboot_uri = {
    .uri     = "/api/reboot",
    .method  = HTTP_POST,
    .handler = reboot_post_handler,
};

static const httpd_uri_t mode_uri = {
    .uri     = "/api/mode",
    .method  = HTTP_POST,
    .handler = mode_post_handler,
};

static const httpd_uri_t game_state_uri = {
    .uri     = "/api/game/state",
    .method  = HTTP_GET,
    .handler = game_state_get_handler,
};

static const httpd_uri_t game_start_uri = {
    .uri     = "/api/game/start",
    .method  = HTTP_POST,
    .handler = game_start_post_handler,
};

static const httpd_uri_t game_reveal_uri = {
    .uri     = "/api/game/reveal",
    .method  = HTTP_POST,
    .handler = game_reveal_post_handler,
};

static const httpd_uri_t game_reset_uri = {
    .uri     = "/api/game/reset",
    .method  = HTTP_POST,
    .handler = game_reset_post_handler,
};

static const httpd_uri_t game_vote_uri = {
    .uri     = "/api/game/vote",
    .method  = HTTP_POST,
    .handler = game_vote_post_handler,
};

esp_err_t server_start(device_config_t *cfg)
{
    if (s_server != NULL) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }
    s_config = cfg;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;

    esp_err_t err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server: %s", esp_err_to_name(err));
        return err;
    }

    httpd_register_uri_handler(s_server, &root_uri);
    httpd_register_uri_handler(s_server, &host_uri);
    httpd_register_uri_handler(s_server, &player_uri);
    httpd_register_uri_handler(s_server, &status_uri);
    httpd_register_uri_handler(s_server, &display_uri);
    httpd_register_uri_handler(s_server, &config_get_uri);
    httpd_register_uri_handler(s_server, &config_post_uri);
    httpd_register_uri_handler(s_server, &reboot_uri);
    httpd_register_uri_handler(s_server, &mode_uri);
    httpd_register_uri_handler(s_server, &game_state_uri);
    httpd_register_uri_handler(s_server, &game_start_uri);
    httpd_register_uri_handler(s_server, &game_reveal_uri);
    httpd_register_uri_handler(s_server, &game_reset_uri);
    httpd_register_uri_handler(s_server, &game_vote_uri);

    ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
    return ESP_OK;
}

esp_err_t server_stop(void)
{
    if (s_server == NULL) {
        return ESP_OK;
    }

    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    return err;
}
