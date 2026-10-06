# A KiwiSDR or a Web-888 over KiwiSDR's protocol: the kiwi firmware's radio.h
# (VFO_RADIO=kiwi) -- one of the configuration page's receivers listened to
# and tuned, another a swipe away, with no restart. The protocol itself is
# components/kiwi_proto, the list of receivers components/sdr_rx's.
idf_component_register(
    SRCS "kiwi_client.c"
    INCLUDE_DIRS "include"
    REQUIRES radio
    PRIV_REQUIRES kiwi_proto sdr_rx vfo_core audio esp_timer nvs_flash
)
