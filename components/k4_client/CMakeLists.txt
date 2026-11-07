# An Elecraft K4 over its remote protocol: the k4 firmware's implementation of
# radio.h (VFO_RADIO=k4). k4_parse.c is pure C, the host tests' too
# (test/host); k4_sha.c is the one piece that needs a hash.
idf_component_register(
    SRCS "k4_client.c" "k4_parse.c" "k4_sha.c"
    INCLUDE_DIRS "."
    REQUIRES radio vfo_core audio esp_timer esp_system lwip
    PRIV_REQUIRES esphome__micro-opus mbedtls esp_wifi
)
