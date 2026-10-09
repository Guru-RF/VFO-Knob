# A WebSDR -- PA3FWM's server software, the receivers on websdr.org: the
# websdr firmware's radio.h (VFO_RADIO=websdr) -- one of the configuration
# page's receivers listened to, the dial running on from one of its bands
# into the next, BAND among its bands or its band plan, another receiver a
# swipe away, with no restart. The protocol and the session are
# components/wsdr_proto, the receivers net_prov's list; an address is read
# as the OpenWebRX firmware reads one (owrx_url).
idf_component_register(
    SRCS "wsdr_client.c"
    INCLUDE_DIRS "include"
    REQUIRES radio
    PRIV_REQUIRES wsdr_proto owrx_proto websdr_link kiwi_proto net_prov vfo_core audio esp_timer nvs_flash
)
