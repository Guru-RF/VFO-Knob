# The setup firmware has no radio: radio.h answered with nothing, for the
# parts of the knob that ask. Its WiFi setup and firmware picker are in
# main/app_main.c (VFO_RADIO_SETUP), beside the installer they use.
idf_component_register(
    SRCS "setup_client.c"
    REQUIRES radio
)
