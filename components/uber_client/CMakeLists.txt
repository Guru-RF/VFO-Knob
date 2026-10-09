# An UberSDR over its own protocol: the ubersdr firmware's implementation of
# radio.h (VFO_RADIO=ubersdr) -- a web receiver, listened to, with its spots,
# its voices and its SSTV gallery (uber.h); the next in the knob's list of
# receivers (net_prov) when the one in use cannot be reached.
idf_component_register(
    SRCS "uber_client.c" "uber_aux.c" "uber_net.c" "uber_json.c" "uber_png.c"
    INCLUDE_DIRS "include"
    REQUIRES radio
    PRIV_REQUIRES vfo_core audio esp_timer esp_system esp_app_format esp-tls lwip nvs_flash kvstore
                  mbedtls esp_rom esphome__micro-opus net_prov
)
