idf_component_register(
    SRCS "webcfg.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server net_prov tci_client usb_net ui audio vfo_core esp_app_format
    EMBED_TXTFILES "www/index.html"
)
