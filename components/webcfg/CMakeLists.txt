# The chosen radio's client: see VFO_RADIO in the top-level CMakeLists.txt.
idf_build_get_property(radio_client VFO_RADIO_CLIENT)
# The setup firmware's WiFi setup page, for a phone on its hotspot.
set(_pages "www/index.html" "www/radio.html")
if(radio_client STREQUAL "setup_client")
    list(APPEND _pages "www/portal.html")
endif()
idf_component_register(
    SRCS "webcfg.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server net_prov radio ${radio_client} usb_net ui audio ota vfo_core esp_app_format mbedtls lwip espcoredump spi_flash sdr_rx bt_link
    EMBED_TXTFILES ${_pages}
)
