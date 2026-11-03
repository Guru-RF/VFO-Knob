# The ADS-B Radar firmware's client: the feed of the aircraft round the
# radar's middle, adsbdb for a flight's route, and the radar's settings on
# the configuration page (adsb_web.c) (VFO_RADIO=adsb). After
# Alex-developer's ADSB-Radar (LICENSE). The map's tables are in the image,
# from tools/adsb_tables.py.
idf_component_register(
    SRCS "adsb_client.c" "adsb_web.c"
    INCLUDE_DIRS "include"
    REQUIRES radio vfo_core net_prov esp_http_client esp_http_server mbedtls esp_timer esp_app_format
    EMBED_FILES "tables/map.bin" "tables/airports.bin"
)
