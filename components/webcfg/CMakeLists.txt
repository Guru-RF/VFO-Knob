# The chosen radio's client: see VFO_RADIO in the top-level CMakeLists.txt.
idf_build_get_property(radio_client VFO_RADIO_CLIENT)
idf_component_register(
    SRCS "webcfg.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server net_prov radio ${radio_client} usb_net ui audio ota vfo_core esp_app_format mbedtls lwip espcoredump spi_flash
    EMBED_TXTFILES "www/index.html"
)
