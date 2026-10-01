idf_component_register(
    SRCS "ui.c" "splash.c" "rfguru_logo.c" "font_mic_14.c" "font_svx_icons_24.c" "font_btmic_28.c"
    INCLUDE_DIRS "include"
    REQUIRES panel hal_touch lvgl esp_lvgl_port net_prov
)
