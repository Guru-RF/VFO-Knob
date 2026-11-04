# The Weather firmware's client: Open-Meteo's forecast for the page's place,
# asked every quarter of an hour or so, and the place and units on the
# configuration page (weather_web.c) (VFO_RADIO=weather). Weather data by
# Open-Meteo.com, CC BY 4.0.
idf_component_register(
    SRCS "weather_client.c" "weather_web.c"
    INCLUDE_DIRS "include"
    REQUIRES radio vfo_core net_prov esp_http_client esp_http_server mbedtls esp_timer esp_app_format
)
