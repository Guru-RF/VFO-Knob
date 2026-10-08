# The web SDRs' link: a WebSocket to a receiver, in the clear or over TLS
# (kiwi_proto's kiwi_tls), its frames of any length read a piece at a time,
# an HTTP GET beside it, and the audio's way into the ring -- what
# OpenWebRX's session runs on (components/owrx_proto's owrx_sess.c).
idf_component_register(
    SRCS "websdr_link.c" "wl_flow.c"
    INCLUDE_DIRS "include"
    REQUIRES kiwi_proto
    PRIV_REQUIRES esp_timer log lwip
)
