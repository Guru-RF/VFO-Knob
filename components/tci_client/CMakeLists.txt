# AetherSDR over TCI: the aethersdr firmware's implementation of radio.h --
# and openx6x00's, jupitersdr's, and openx6bt's over Bluetooth (ws_bt).
idf_component_register(
    SRCS "tci_client.c"
    REQUIRES radio vfo_core audio net_prov esp_websocket_client esp_timer esp_system ws_bt esp_event esp_app_format
)
