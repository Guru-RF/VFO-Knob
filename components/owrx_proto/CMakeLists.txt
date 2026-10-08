# OpenWebRX's protocol (OpenWebRX and OpenWebRX+). owrx_proto.c and
# owrx_adpcm.c are plain C with no ESP-IDF in them -- an address, what the
# knob says, what the receiver says (its messages read as they stream, its
# status.json), its audio decoded -- so test/host builds and checks them on
# the PC; owrx_sess.c is a session on the wire and a status.json read, on the
# web SDRs' link (components/websdr_link).
idf_component_register(
    SRCS "owrx_proto.c" "owrx_adpcm.c" "owrx_sess.c"
    INCLUDE_DIRS "include"
    REQUIRES websdr_link
    PRIV_REQUIRES esp_timer log
)
