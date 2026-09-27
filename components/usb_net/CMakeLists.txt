idf_component_register(
    SRCS "usb_net.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif esp_event lwip
)
