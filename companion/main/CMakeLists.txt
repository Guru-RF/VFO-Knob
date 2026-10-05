# air.c measures the air to the headset where only the stack can see it: it
# reads Bluedroid's own headers, which the bt component keeps to itself --
# here from its private directories, read with this build's sdkconfig and
# definitions, as the stack itself was built -- and puts itself in front of
# four of the stack's functions with the linker's --wrap. a2dp.c counts a
# speaker's packets so, in front of a fifth, stands in for the stack's A2DP
# sink, which this chip never is, and hears a speaker's answers the stack
# drops.
set(bluedroid "$ENV{IDF_PATH}/components/bt/host/bluedroid")
idf_component_register(
    SRCS "main.c" "link.c" "hfp.c" "rs.c" "air.c" "upd.c" "a2dp.c" "kind.c" "batt.c"
    INCLUDE_DIRS "." "../../components/bt_link/include"
    PRIV_INCLUDE_DIRS "$ENV{IDF_PATH}/components/bt/common/include"
                      "$ENV{IDF_PATH}/components/bt/common/osi/include"
                      "$ENV{IDF_PATH}/components/bt/common/btc/include"
                      "${bluedroid}/common/include"
                      "${bluedroid}/stack/include"
                      "${bluedroid}/stack/btm/include"
                      "${bluedroid}/bta/include"
                      "${bluedroid}/hci/include"
                      "${bluedroid}/btc/profile/std/include"
                      "${bluedroid}/btc/include"
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
#
# Through a2dp.c: each packet of a speaker's SBC frames leaving the stack's
# queue for the air (called from bta_av_co.c, the function in
# btc_a2dp_source.c) -- counted for the speaker's reports, nothing more.
#
# And the stack's A2DP sink, which it builds beside the source, left out:
# a2dp.c stands in for each of its entry points the rest of the stack calls
# (from btc_av.c and btc_a2dp.c, the functions in btc_a2dp_sink.c), so that
# nothing calls the real ones -- and the linker, which drops what nothing
# calls, drops the sink with its SBC decoder and their 9.7 kB of static RAM.
# The sink is never started here: this chip is a speaker's source.
#
# And the stack's reader of a remote control's answers (called from
# btc_avrc.c, the function in avrc_pars_ct.c): a speaker's INTERIM answer,
# its volume as it stands, which the stack drops, a2dp.c hears.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=bta_ag_sco_co_in_data"
                                                 "-Wl,--wrap=bta_ag_sco_co_open"
                                                 "-Wl,--wrap=bta_ag_sco_co_close"
                                                 "-Wl,--wrap=btm_sco_process_num_completed_pkts"
                                                 "-Wl,--wrap=btc_a2dp_source_audio_readbuf"
                                                 "-Wl,--wrap=btc_a2dp_sink_startup"
                                                 "-Wl,--wrap=btc_a2dp_sink_shutdown"
                                                 "-Wl,--wrap=btc_a2dp_sink_on_idle"
                                                 "-Wl,--wrap=btc_a2dp_sink_on_stopped"
                                                 "-Wl,--wrap=btc_a2dp_sink_on_suspended"
                                                 "-Wl,--wrap=btc_a2dp_sink_set_rx_flush"
                                                 "-Wl,--wrap=btc_a2dp_sink_reset_decoder"
                                                 "-Wl,--wrap=btc_a2dp_sink_reg_data_cb"
                                                 "-Wl,--wrap=btc_a2dp_sink_enque_buf"
                                                 "-Wl,--wrap=AVRC_ParsResponse")
