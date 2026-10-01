# The firmware images on the knob's microSD card: see include/sd_cache.h.
idf_component_register(
    SRCS "sd_cache.c"
    INCLUDE_DIRS "include"
    REQUIRES fatfs
    PRIV_REQUIRES board sdmmc esp_driver_sdmmc mbedtls
)
