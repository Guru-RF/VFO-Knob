# KiwiSDR's protocol, shared by everything that speaks it: the kiwi firmware's
# receiver (components/kiwi_client) and the web SDR beside a radio
# (components/sdr_rx). kiwi_proto.c is plain C with no ESP-IDF in it, so
# test/host builds and checks it on the PC; kiwi_mark.c keeps the receivers'
# day-limit marks in NVS; kiwi_sess.c is a session on the wire, a /status
# read and the page's Test; kiwi_tls.c their TLS, for a receiver behind a
# front that speaks only https:// -- mbedTLS with the certificate bundle. What
# nothing calls is left out of the image.
idf_component_register(
    SRCS "kiwi_proto.c" "kiwi_mark.c" "kiwi_sess.c" "kiwi_tls.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash kvstore
    PRIV_REQUIRES esp_timer log lwip mbedtls
)
