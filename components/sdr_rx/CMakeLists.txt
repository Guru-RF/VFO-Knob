idf_component_register(
    SRCS "sdr_rx.c"
    INCLUDE_DIRS "include"
    REQUIRES audio nvs_flash esp_timer kiwi_proto
)
