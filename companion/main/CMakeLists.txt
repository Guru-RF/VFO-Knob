idf_component_register(
    SRCS "main.c" "link.c" "hfp.c" "rs.c"
    INCLUDE_DIRS "." "../../components/bt_link/include"
    REQUIRES driver esp_driver_uart esp_driver_gpio nvs_flash bt esp_timer esp_app_format
)
