idf_component_register(
    SRCS "server.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "index.html" "host.html" "player.html"
    REQUIRES esp_http_server display config stocks game esp_wifi esp_netif esp_timer esp_app_format
    PRIV_REQUIRES espressif__cjson
)
