idf_component_register(
    SRCS "app_main.c" "gpio_scan.c"
    INCLUDE_DIRS "."
    REQUIRES board vfo_core hal_haptic hal_encoder panel hal_touch ui net_prov tci_client audio esp_psram driver esp_timer esp_driver_usb_serial_jtag
)
