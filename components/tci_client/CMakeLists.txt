# AetherSDR over TCI: the aethersdr firmware's implementation of radio.h.
idf_component_register(
    SRCS "tci_client.c"
    REQUIRES radio vfo_core audio esp_websocket_client esp_timer esp_system
)
