idf_component_register(
    SRCS "app_main.c" "gpio_scan.c"
    INCLUDE_DIRS "."
    REQUIRES board vfo_core hal_haptic hal_encoder panel net_prov tci_client esp_psram driver esp_timer
)
