# The EpiBeam firmware's client: EpiCenter, the station's brain, over a
# WebSocket -- to come. Until then radio.h answered with nothing, as the setup
# firmware's is, and the beam's direction kept where it will be sent from.
idf_component_register(
    SRCS "epi_client.c"
    INCLUDE_DIRS "include"
    REQUIRES radio vfo_core
)
