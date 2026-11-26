# The TailChaser Tag firmware's client (VFO_RADIO=tctag): there is no radio.
# The knob's own microphone listens to the room, where a TailChaser repeater
# is played, and the decoder (vfo_core tag.c) reads the callsign tag under
# its audio: who is talking, kept as one entry per over (tag_hist.c) on the
# SD card. radio.h answers for a receiver always ready, so nothing says NO
# LINK over the face, and the face is who was heard (ui.c TAG_FACE); the
# configuration page has the list too (tctag_web.c). Each new over's callsign
# can be said out loud, in a voice ElevenLabs makes (tctag_say.c).
idf_component_register(
    SRCS "tctag_client.c" "tctag_web.c" "tctag_say.c"
    INCLUDE_DIRS "include"
    REQUIRES radio vfo_core audio kvstore wallclock esp_timer esp_http_server
             esp_http_client mbedtls bt_link sd_cache fatfs
)
