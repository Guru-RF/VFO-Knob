idf_component_register(
    SRCS "tailchaser_client.c" "tc_proto.c" "tc_http.c" "tc_ts.c" "tc_web.c" "tc_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES radio
    PRIV_REQUIRES vfo_core esp_timer esp_netif lwip json kvstore esp_http_server microlink audio
)
