# A telephone over WiFi: the phone firmware's implementation of radio.h
# (VFO_RADIO=phone). Its own small SIP user agent (sip.c), G.722 (g722.c, HD
# voice, 16 kHz as the knob's audio is) and G.711 (g711.c) for one account; the face is SVXConnect's, the favourites in the talkgroups'
# place; the page's API in phone_web.c.
idf_component_register(
    SRCS "phone_client.c" "sip.c" "g722.c" "g711.c" "phone_web.c" "contacts.c"
    INCLUDE_DIRS "include"
    REQUIRES radio
    PRIV_REQUIRES vfo_core audio bt_link esp_timer esp_netif lwip nvs_flash
                  esp_http_server esp_rom esp_http_client esp-tls mbedtls json net_prov
)
