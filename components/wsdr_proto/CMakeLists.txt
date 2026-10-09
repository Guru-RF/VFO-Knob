# PA3FWM's WebSDR protocol. wsdr_proto.c and wsdr_codec.c are plain C with no
# ESP-IDF in them -- its bandinfo.js read as it streams, the tuning the knob
# sends, its audio items decoded -- so test/host builds and checks them on
# the PC; wsdr_sess.c is a session on the wire and the files a page reads,
# on the web SDRs' link (components/websdr_link).
idf_component_register(
    SRCS "wsdr_proto.c" "wsdr_codec.c" "wsdr_sess.c"
    INCLUDE_DIRS "include"
    REQUIRES websdr_link
    PRIV_REQUIRES esp_timer log
)
