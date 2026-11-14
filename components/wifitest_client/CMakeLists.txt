# The WiFi Test firmware's client (VFO_RADIO=wifitest): there is no radio.
# radio.h answers for one always ready, so nothing says NO LINK over the
# face, and the face is the test (ui.c WIFITEST_FACE). It measures the
# network the knob is on -- the signal, the drops and their reasons, pings to
# the router, the radios' hosts and the internet, the names, the channels
# round it -- and shows it on the configuration page (wifitest_web.c) and as
# a report to paste.
idf_component_register(
    SRCS "wifitest_client.c" "wifitest_web.c"
    INCLUDE_DIRS "include"
    REQUIRES radio vfo_core net_prov esp_wifi esp_netif esp_event esp_timer esp_http_server esp_app_format lwip mdns
)
