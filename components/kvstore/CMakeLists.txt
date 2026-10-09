idf_component_register(
    SRCS "kv.c" "kv_tlv.c" "kv_merge.c" "kv_card.c" "kv_nvs.c" "kv_writer.c" "kv_ns.c" "kv_test.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_common nvs_flash
    PRIV_REQUIRES sd_cache fatfs esp_timer esp_hw_support esp_app_format esp_system log heap
)
