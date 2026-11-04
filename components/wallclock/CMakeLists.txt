idf_component_register(
    SRCS "wallclock.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_netif esp_timer
)
