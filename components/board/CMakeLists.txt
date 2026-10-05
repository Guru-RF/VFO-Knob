idf_component_register(
    SRCS "board_init.c" "board_power.c" "knob_batt.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_i2c esp_driver_gpio esp_adc
    PRIV_REQUIRES esp_timer
)
