idf_component_register(
    SRCS "net_prov.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_event nvs_flash lwip mdns
)
