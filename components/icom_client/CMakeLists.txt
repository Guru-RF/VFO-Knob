# An IC-705 over WiFi: the icom firmware's implementation of radio.h.
idf_component_register(
    SRCS "icom_client.c"
    REQUIRES radio vfo_core audio esp_timer esp_system lwip nvs_flash kvstore
)
