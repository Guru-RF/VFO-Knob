# A FlexRadio over its own API, as a MultiFlex station: the multiflex
# firmware's implementation of radio.h -- the radios on the LAN found by
# their discovery broadcast, and SmartLink's away from home. flex_parse.c is
# pure C, the host tests' too (test/host).
idf_component_register(
    SRCS "flex_client.c" "smartlink.c" "discovery.c" "flex_parse.c"
    INCLUDE_DIRS "include"
    REQUIRES radio vfo_core audio esp_timer esp_system lwip nvs_flash esp-tls esp_http_server
    PRIV_REQUIRES esphome__micro-opus esp_http_client json mbedtls net_prov
)
