idf_component_register(
    SRCS "panel.c" "sh8601_init_cmds.c"
    INCLUDE_DIRS "include"
    REQUIRES board esp_lcd esp_driver_spi esp_driver_ledc esp_lcd_sh8601
)
