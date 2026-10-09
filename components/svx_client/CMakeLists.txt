# An SvxLink reflector over WiFi: the svxconnect firmware's implementation of
# radio.h (VFO_RADIO=svxconnect). A port of SVXConnect-CLI's reflector client:
# svx/ holds the CLI's own protocol, talkgroup manager, node info and codec,
# laid out as in its source tree; the rest is the knob's side of them.
idf_component_register(
    SRCS "svx_client.c" "svx_net.c" "svx_udp.c" "svx_pki.c" "svx_portal.c" "svx_feed.c"
         "svx_web.c"
         "svx/common/proto.c" "svx/common/util.c" "svx/audio/codec.c"
         "svx/tg/tgmanager.c" "svx/reflector/nodeinfo.c"
    PRIV_INCLUDE_DIRS "svx" "svx/common" "svx/audio"
    REQUIRES radio
    PRIV_REQUIRES vfo_core audio esp_timer esp_netif lwip mbedtls nvs_flash kvstore
                  esp_http_client esp_http_server json esphome__micro-opus
                  esp_websocket_client
)
# The CLI's files build unchanged. Its printf formats give uint32_t as %u,
# which is unsigned long on this compiler: the same 32 bits, another type.
set_source_files_properties(svx/common/proto.c svx/tg/tgmanager.c svx/audio/codec.c
                            svx/reflector/nodeinfo.c
                            PROPERTIES COMPILE_OPTIONS "-Wno-format")
# What the reflector's node list calls the knob, and the Opus encoder's
# complexity. 5 measured 11.7 ms per 20 ms frame on the knob, 19.3 at worst --
# too close to the frame -- so 3 (the log line "over: ..." after each
# transmission says what it costs).
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    SVX_CLIENT_NAME="SVXConnect-Knob" SVX_OPUS_COMPLEXITY=3)
