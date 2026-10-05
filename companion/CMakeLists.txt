# The knob's second chip: the ESP32 (ESP32-U4WDH) beside the ESP32-S3, with
# classic Bluetooth, which the S3 has not. This firmware makes it the audio
# gateway for a Bluetooth headset and the music source for a speaker, and
# talks to the S3 over the UART between them
# (components/bt_link/include/bt_link_proto.h, shared with the knob).
#
#   idf.py -B build build               (IDF 5.5, target esp32)
#
# Every build is signed with the S3's key (../ota_signing_key.pem) and can be
# updated: the knob updates this chip over that UART by itself (main/upd.c).
# A cable writes it once per knob, at the bench: the bootloader (with
# rollback), the partition table, otadata erased, a first image --
# tools/install-setup.sh, or on a knob in use, its headset's pairing kept,
# tools/install-setup.sh --second-chip. After that, a development image by
# cable is
#
#   idf.py -B build -p /dev/ttyUSB0 app-flash
#   esptool.py -p /dev/ttyUSB0 erase_region 0xe000 0x2000
#
# the second line so that the image just written at ota_0 is the one that
# starts. Never a plain `flash`: it writes the bootloader and the partition
# table again, which only the bench writes.
#
# It is reached over the knob's USB-C with the plug the other way round from
# the S3's: there the CH340 on the ESP32's UART0 answers, and esptool resets
# the chip into its loader through it.
#
# -D COMPANION_DEFAULTS=<file> adds an overlay to the defaults: the release's
# and the bench's test images set their version and the release mark so.
cmake_minimum_required(VERSION 3.16)
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults" ${COMPANION_DEFAULTS})
if(NOT DEFINED SDKCONFIG)
    set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Bluedroid sizes its table of security records by the profiles built in, and
# gives the audio gateway one -- but it registers two servers (the headset and
# the hands-free profiles) and, calling a headset, needs a record more. Built
# without the hands-free client, whose record covered for it, the call fails:
# "Out of Service Records", then "MX service not found". Room for all three,
# and for A2DP's: its AVDTP registers up to three more, its AVCTP up to two
# (384 bytes, the six more).
idf_build_set_property(COMPILE_DEFINITIONS "BTM_SEC_MAX_SERVICE_RECORDS=12" APPEND)
project(vfo-knob-companion)
# The defaults only seed a new sdkconfig: one made before the updates keeps
# its unsigned, rollback-less settings, and its image -- once on a chip --
# would be a firmware that can never go back, nor check what it is sent. Nor
# one whose start turns off the bootloader's RTC watchdog before app_main
# (BOOTLOADER_WDT_DISABLE_IN_USER_CODE unset): a firmware hung on trial would
# then never go back.
if(NOT CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE OR NOT CONFIG_BOOTLOADER_PROJECT_VER EQUAL 2
   OR NOT CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME OR NOT CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT
   OR NOT CONFIG_BOOTLOADER_WDT_DISABLE_IN_USER_CODE OR CONFIG_ESP_MAIN_TASK_STACK_SIZE LESS 8192)
    message(FATAL_ERROR "this build directory's sdkconfig predates the second chip's updates -- "
                        "delete it (defaults only seed a new one)")
endif()
# Nor one made before speakers: built without A2DP, a firmware that tells the
# knob it plays to speakers (main.c's hello) would have nothing to play them.
if(NOT CONFIG_BT_A2DP_ENABLE)
    message(FATAL_ERROR "this build directory's sdkconfig predates speakers (no CONFIG_BT_A2DP_ENABLE) -- "
                        "delete it (defaults only seed a new one)")
endif()
