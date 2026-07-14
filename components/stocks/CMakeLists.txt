idf_component_register(
    SRCS "stocks.c"
    INCLUDE_DIRS "."
    REQUIRES config display esp_http_client mbedtls esp_timer
    PRIV_REQUIRES espressif__cjson
)
