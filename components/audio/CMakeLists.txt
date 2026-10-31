idf_component_register(
    SRCS "audio_out.c" "audio_in.c" "jack_level.c"
    INCLUDE_DIRS "include"
    REQUIRES board esp_driver_i2s esp_driver_gpio
)
