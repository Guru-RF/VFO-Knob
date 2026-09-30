# A FlexRadio over its own API, as a MultiFlex station: the multiflex
# firmware's implementation of radio.h.
idf_component_register(
    SRCS "flex_client.c" "smartlink.c"
    INCLUDE_DIRS "include"
    REQUIRES radio vfo_core audio esp_timer esp_system lwip nvs_flash esp-tls esp_http_server
    PRIV_REQUIRES esphome__micro-opus esp_http_client json mbedtls
)
