# The chosen radio's client: see VFO_RADIO in the top-level CMakeLists.txt.
idf_build_get_property(radio_client VFO_RADIO_CLIENT)
# The WiFi setup page, for a phone on the knob's hotspot: the setup
# firmware's, and every firmware's with none of its networks in reach.
set(_pages "www/index.html" "www/radio.html" "www/portal.html")
# The OpenWebRX and WebSDR firmwares' receivers are kept by their addresses
# whole, read as their clients read them (owrx_url).
set(_more)
if(radio_client STREQUAL "owrx_client" OR radio_client STREQUAL "wsdr_client")
    list(APPEND _more owrx_proto)
endif()
idf_component_register(
    SRCS "webcfg.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server net_prov radio ${radio_client} usb_net ui audio ota vfo_core esp_app_format mbedtls lwip espcoredump spi_flash sdr_rx kiwi_proto bt_link board ${_more}
    EMBED_TXTFILES ${_pages}
)
