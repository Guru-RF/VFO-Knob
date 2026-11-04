# The Clock firmware's client (VFO_RADIO=clock): there is no radio, and
# nothing to ask but the time, which is SNTP's (components/wallclock). radio.h
# answers for a radio always ready and never transmitting, so nothing says NO
# LINK over the face, and the face is the clock (ui.c CLOCK_FACE). Its alarms
# are set on the knob or on the configuration page (clock_web.c), and beep on
# a Bluetooth headset or speaker, or on the jack, while they ring.
idf_component_register(
    SRCS "clock_client.c" "clock_web.c"
    INCLUDE_DIRS "include"
    REQUIRES radio vfo_core net_prov audio bt_link wallclock esp_http_server
)
