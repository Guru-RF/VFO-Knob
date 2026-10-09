idf_component_register(
    SRCS "net_prov.c" "net_ap.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event nvs_flash lwip mdns vfo_core kvstore
)
