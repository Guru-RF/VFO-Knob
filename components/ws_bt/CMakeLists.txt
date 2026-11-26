# A WebSocket client over a radio's Bluetooth serial link, through the knob's
# second chip (bt_link): the openx6bt firmware's TCI. Built for every
# firmware, compiled empty but for VFO_BT_RADIO -- and for icombt's, whose
# radio speaks CI-V.
idf_component_register(
    SRCS "ws_bt.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common freertos
    PRIV_REQUIRES bt_link esp_timer esp_hw_support mbedtls
)
