cmake_minimum_required(VERSION 3.16)

# ESP-IDF only picks up sdkconfig.defaults and sdkconfig.defaults.<target> on
# its own. sdkconfig.defaults.local (gitignored, holds the WiFi PSK and the
# AetherSDR host) has to be named explicitly or it is silently ignored -- which
# looks exactly like credentials that do not work.
set(_vfo_defaults "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
    list(APPEND _vfo_defaults "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
    message(STATUS "VFO: using sdkconfig.defaults.local")
endif()
# Honour a caller-supplied list (used by the USB-networking overlay) rather
# than clobbering it.
if(NOT DEFINED SDKCONFIG_DEFAULTS)
    set(SDKCONFIG_DEFAULTS "${_vfo_defaults}")
endif()

# Keep each build directory's configuration to itself. By default IDF writes
# sdkconfig next to the project, so `idf.py -B build_usbnet` and `idf.py -B
# build` share one file: whichever ran last wins, and SDKCONFIG_DEFAULTS is
# only consulted when that file does not yet exist. The failure is silent --
# a USB-networking build comes out with USB networking compiled out, reports
# success, flashes, and simply does nothing.
if(NOT DEFINED SDKCONFIG)
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
endif()

# The radio this firmware is for: `idf.py -D VFO_RADIO=...`. There is one
# firmware per radio -- they share everything but the radio's client, and
# internal RAM has room for one -- and the image is named for it,
# vfo-knob-<radio>. That name is how a knob tells its own updates from another
# radio's firmware, and which update channel it follows (components/ota).
set(VFO_RADIO "aethersdr" CACHE STRING "The radio this firmware is built for")
set(_vfo_radios aethersdr icom svxconnect multiflex)
if(NOT VFO_RADIO IN_LIST _vfo_radios)
    message(FATAL_ERROR "VFO_RADIO=${VFO_RADIO}: there is firmware for ${_vfo_radios}")
endif()

# Each radio's client implements components/radio/include/radio.h, so exactly
# one may be built: two would be two definitions of everything in it.
# svxconnect is not a radio but an SvxLink reflector: its client speaks the
# reflector protocol, and the dial picks talkgroups instead of frequencies.
# multiflex speaks a FlexRadio's own API, with no computer in between: the
# knob is one of the radio's MultiFlex stations.
if(VFO_RADIO STREQUAL "aethersdr")
    set(_vfo_client tci_client)
elseif(VFO_RADIO STREQUAL "svxconnect")
    set(_vfo_client svx_client)
elseif(VFO_RADIO STREQUAL "multiflex")
    set(_vfo_client flex_client)
else()
    set(_vfo_client icom_client)
endif()
foreach(_c tci_client icom_client svx_client flex_client)
    if(NOT _c STREQUAL _vfo_client)
        list(APPEND EXCLUDE_COMPONENTS ${_c})
    endif()
endforeach()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# The component manager records what it resolved in a lock file, and the two
# firmwares resolve different sets -- the icom one has no WebSocket client --
# so a shared file was rewritten by every build of the other radio, and a
# release refuses a tree that has changed. The AetherSDR firmware keeps
# dependencies.lock; any other radio's has its own.
if(NOT VFO_RADIO STREQUAL "aethersdr")
    idf_build_set_property(DEPENDENCIES_LOCK dependencies.${VFO_RADIO}.lock)
endif()
# For components that call the client (main, webcfg), and for code that looks
# different per radio: VFO_RADIO_AETHERSDR, VFO_RADIO_ICOM,
# VFO_RADIO_SVXCONNECT or VFO_RADIO_MULTIFLEX is defined as 1.
idf_build_set_property(VFO_RADIO_CLIENT ${_vfo_client})
string(TOUPPER "${VFO_RADIO}" _vfo_radio_uc)
idf_build_set_property(COMPILE_DEFINITIONS "VFO_RADIO_${_vfo_radio_uc}=1" APPEND)
project(vfo-knob-${VFO_RADIO})
