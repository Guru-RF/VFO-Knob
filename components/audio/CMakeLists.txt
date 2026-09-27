idf_component_register(
    SRCS "audio_out.c"
    INCLUDE_DIRS "include"
    REQUIRES board esp_driver_i2s esp_driver_gpio
)
