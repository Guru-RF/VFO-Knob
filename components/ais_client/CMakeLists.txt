# The AIS Radar firmware's client: the ships round the radar's middle, from
# AISStream.io's stream (ais_ws.c, a WebSocket over components/kiwi_proto's
# TLS, inflated by the S3 ROM's tinfl) or an AIS-catcher of one's own, and
# the radar's settings on the configuration page (ais_web.c) (VFO_RADIO=ais).
# The map: ADS-B Radar's coasts and borders, and the harbours
# (tools/ais_tables.py), both in the image.
idf_component_register(
    SRCS "ais_client.c" "ais_ws.c" "ais_web.c"
    INCLUDE_DIRS "include"
    REQUIRES radio vfo_core net_prov esp_http_client esp_http_server mbedtls esp_timer esp_app_format
             kiwi_proto esp_rom lwip
    EMBED_FILES "../adsb_client/tables/map.bin" "tables/ports.bin"
)
