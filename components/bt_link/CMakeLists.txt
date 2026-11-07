# The link to the knob's second chip, the ESP32 with classic Bluetooth that
# runs the companion firmware (companion/): a Bluetooth headset for the knob.
# bt_level.c, a device's level, and bt_known.c, the devices connected before,
# are plain C: test/host tests them.
idf_component_register(
    SRCS "bt_link.c" "bt_level.c" "bt_known.c"
    INCLUDE_DIRS "include"
    REQUIRES board audio
    PRIV_REQUIRES esp_driver_uart esp_timer nvs_flash
)
