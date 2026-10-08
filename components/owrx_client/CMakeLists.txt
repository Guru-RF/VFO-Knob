# An OpenWebRX or an OpenWebRX+ web receiver: the openwebrx firmware's
# radio.h (VFO_RADIO=owrx) -- one of the configuration page's receivers
# listened to and tuned on the band it is on, another band chosen from its
# list, another receiver a swipe away, with no restart. The protocol and the
# session are components/owrx_proto, the receivers net_prov's list.
idf_component_register(
    SRCS "owrx_client.c"
    INCLUDE_DIRS "include"
    REQUIRES radio
    PRIV_REQUIRES owrx_proto websdr_link kiwi_proto net_prov vfo_core audio esp_timer nvs_flash
)
