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

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(vfo-knob)
