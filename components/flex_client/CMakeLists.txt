# A FlexRadio over its own API, as a MultiFlex station: the multiflex
# firmware's implementation of radio.h.
idf_component_register(
    SRCS "flex_client.c"
    REQUIRES radio vfo_core audio esp_timer esp_system lwip nvs_flash
    PRIV_REQUIRES esphome__micro-opus
)
