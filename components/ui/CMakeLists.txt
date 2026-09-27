idf_component_register(
    SRCS "ui.c" "splash.c" "rfguru_logo.c"
    INCLUDE_DIRS "include"
    REQUIRES panel hal_touch lvgl esp_lvgl_port
)
