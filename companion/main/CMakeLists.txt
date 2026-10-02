# air.c measures the air to the headset where only the stack can see it: it
# reads Bluedroid's own headers, which the bt component keeps to itself --
# here from its private directories, read with this build's sdkconfig and
# definitions, as the stack itself was built -- and puts itself in front of
# four of the stack's functions with the linker's --wrap.
set(bluedroid "$ENV{IDF_PATH}/components/bt/host/bluedroid")
idf_component_register(
    SRCS "main.c" "link.c" "hfp.c" "rs.c" "air.c" "upd.c"
    INCLUDE_DIRS "." "../../components/bt_link/include"
    PRIV_INCLUDE_DIRS "$ENV{IDF_PATH}/components/bt/common/include"
                      "$ENV{IDF_PATH}/components/bt/common/osi/include"
                      "${bluedroid}/common/include"
                      "${bluedroid}/stack/include"
                      "${bluedroid}/stack/btm/include"
                      "${bluedroid}/bta/include"
                      "${bluedroid}/hci/include"
    REQUIRES driver esp_driver_uart esp_driver_gpio nvs_flash bt esp_timer esp_app_format
             app_update esp_partition esp_bootloader_format mbedtls hal
)
# Through air.c on their way: each of the headset's packets, the audio path's
# opening and closing (all three called from bta_ag_sco.c, the functions in
# bta_ag_co.c), and each completed-packets event (called from btu_hcif.c, the
# function in btm_sco.c). Each is called from another object file than its
# own, so the call can be redirected. The opening and closing do more than
# count: they keep the link out of sniff for the call (air.c). Drop those two
# wraps and a call sleeps at the stack's sniff intervals again, with the
# losses of 2026-10-02.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=bta_ag_sco_co_in_data"
                                                 "-Wl,--wrap=bta_ag_sco_co_open"
                                                 "-Wl,--wrap=bta_ag_sco_co_close"
                                                 "-Wl,--wrap=btm_sco_process_num_completed_pkts")
