idf_component_register(
    SRCS "tci_client.c"
    INCLUDE_DIRS "include"
    REQUIRES vfo_core audio esp_websocket_client esp_timer esp_system
)
