# The knob's second chip: the ESP32 (ESP32-U4WDH) beside the ESP32-S3, with
# classic Bluetooth, which the S3 has not. This firmware makes it the audio
# gateway for a Bluetooth headset, and talks to the S3 over the UART between
# them (components/bt_link/include/bt_link_proto.h, shared with the knob).
#
#   idf.py -B build build               (IDF 5.5, target esp32)
#   idf.py -B build -p /dev/ttyUSB0 flash monitor
#
# It is reached over the knob's USB-C with the plug the other way round from
# the S3's: there the CH340 on the ESP32's UART0 answers, and esptool resets
# the chip into its loader through it.
cmake_minimum_required(VERSION 3.16)
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
if(NOT DEFINED SDKCONFIG)
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Bluedroid sizes its table of security records by the profiles built in, and
# gives the audio gateway one -- but it registers two servers (the headset and
# the hands-free profiles) and, calling a headset, needs a record more. Built
# without the hands-free client, whose record covered for it, the call fails:
# "Out of Service Records", then "MX service not found". Room for all three.
idf_build_set_property(COMPILE_DEFINITIONS "BTM_SEC_MAX_SERVICE_RECORDS=6" APPEND)
project(vfo-knob-companion)
