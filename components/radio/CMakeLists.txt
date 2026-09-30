# The radio interface: the implementation is the chosen radio's client
# (tci_client or icom_client), picked in the top-level CMakeLists.txt. Only
# the defaults for what a client need not have live here.
idf_component_register(SRCS "radio_found.c" INCLUDE_DIRS "include")
