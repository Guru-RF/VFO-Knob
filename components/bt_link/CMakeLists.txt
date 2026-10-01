# The link to the knob's second chip, the ESP32 with classic Bluetooth that
# runs the companion firmware (companion/): a Bluetooth headset for the knob.
idf_component_register(
    SRCS "bt_link.c"
    INCLUDE_DIRS "include"
    REQUIRES board audio
    PRIV_REQUIRES esp_driver_uart esp_timer nvs_flash
)
