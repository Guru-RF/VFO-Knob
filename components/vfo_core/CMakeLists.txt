# Deliberately depends on NOTHING from ESP-IDF. Everything in here also builds
# and runs on the host under test/host/, which is where the anti-echo
# classifier and the PTT FSM are actually debugged.
idf_component_register(
    SRCS
        "src/tci_parse.c"
        "src/echo_ring.c"
        "src/antiecho.c"
        "src/vfo_tune.c"
        "src/ptt_fsm.c"
    INCLUDE_DIRS "include"
)
