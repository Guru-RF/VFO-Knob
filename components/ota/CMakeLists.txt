idf_component_register(
    SRCS "ota.c"
    INCLUDE_DIRS "include"
    REQUIRES app_update esp_https_ota esp_http_client esp-tls mbedtls nvs_flash esp_app_format esp_timer
)
