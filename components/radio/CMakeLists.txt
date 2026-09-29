# The radio interface only: the implementation is the chosen radio's client
# (tci_client or icom_client), picked in the top-level CMakeLists.txt.
idf_component_register(INCLUDE_DIRS "include")
