# Host-side tests for components/vfo_core, the FlexRadio API's parsing, the
# second chip's reading of a device's battery, the knob's own battery, the
# headset's mix and a Bluetooth headset's or speaker's level,
# components/kiwi_proto, components/owrx_proto, and the owners' limits -- the web SDR's and the kiwi
# firmware's receiver's -- against a mock receiver, in the clear and behind
# TLS; and the openwebrx firmware's pages (components/webcfg) in headless
# Chromium against its mock. No cross-compiler; of ESP-IDF only its mbedTLS
# sources, for the receivers' TLS, where its tree is (below).
#
#   cmake -B build_host -S test/host
#   cmake --build build_host
#   ctest --test-dir build_host --output-on-failure
#   ctest --test-dir build_host -L mock --output-on-failure    # the mock alone
#   ctest --test-dir build_host -L tls --output-on-failure     # ...every receiver behind TLS
#
# The tests against the mocks open sockets on 127.0.0.1 only; run them in a
# network namespace of their own to be sure of it:
#   unshare -rn sh -c 'ip link set lo up; ctest --test-dir build_host -j6'

cmake_minimum_required(VERSION 3.16)
project(vfo_core_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# mbedTLS, for the receivers' TLS (components/kiwi_proto/kiwi_tls.c): the
# knob's own, from ESP-IDF's tree -- not a TLS library of the PC's standing
# in for it -- built as the knob has it, TLS 1.2 (shim/mbedtls_cfg), and
# before the sanitizers below: its code is not what is tried here. Without an
# ESP-IDF tree ($IDF_PATH, ~/esp/esp-idf, or -D MBEDTLS_DIR=) the harnesses
# speak in the clear only, and their TLS runs are left out.
set(MBEDTLS_DIR "" CACHE PATH "mbedTLS's sources: ESP-IDF's components/mbedtls/mbedtls")
if(NOT MBEDTLS_DIR)
    foreach(_idf "$ENV{IDF_PATH}" "$ENV{HOME}/esp/esp-idf")
        if(_idf AND EXISTS "${_idf}/components/mbedtls/mbedtls/library/ssl_tls.c")
            set(MBEDTLS_DIR "${_idf}/components/mbedtls/mbedtls")
            break()
        endif()
    endforeach()
endif()
if(MBEDTLS_DIR)
    file(GLOB _mbedtls_srcs "${MBEDTLS_DIR}/library/*.c")
    add_library(mbedtls_host STATIC ${_mbedtls_srcs})
    target_include_directories(mbedtls_host PUBLIC "${MBEDTLS_DIR}/include"
                               "${CMAKE_CURRENT_SOURCE_DIR}/shim/mbedtls_cfg")
    target_include_directories(mbedtls_host PRIVATE "${MBEDTLS_DIR}/library")
    target_compile_definitions(mbedtls_host PUBLIC MBEDTLS_USER_CONFIG_FILE="kiwi_mbedtls_host.h")
    target_compile_options(mbedtls_host PRIVATE -O2 -w)
    set(KIWI_TLS ON)
    message(STATUS "TLS for the kiwi harnesses: mbedTLS from ${MBEDTLS_DIR}")
else()
    set(KIWI_TLS OFF)
    message(STATUS "TLS for the kiwi harnesses: none -- no mbedTLS sources found")
endif()

set(CORE ${CMAKE_CURRENT_SOURCE_DIR}/../../components/vfo_core)

add_library(vfo_core STATIC
    ${CORE}/src/tci_parse.c
    ${CORE}/src/echo_ring.c
    ${CORE}/src/antiecho.c
    ${CORE}/src/vfo_tune.c
    ${CORE}/src/ptt_fsm.c
)
target_include_directories(vfo_core PUBLIC ${CORE}/include)
target_compile_options(vfo_core PRIVATE -Wall -Wextra -Wpedantic -Werror)

# The parser is fed arbitrary bytes off the network, so build the tests with
# sanitizers -- this is where a fuzzer earns its keep.
option(VFO_SANITIZE "Build host tests with ASan/UBSan" ON)
if(VFO_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
endif()

enable_testing()
foreach(t test_tci_parse test_echo_ring test_tune test_ptt_fsm fuzz_tci)
    add_executable(${t} src/${t}.c)
    target_link_libraries(${t} PRIVATE vfo_core)
    target_compile_options(${t} PRIVATE -Wall -Wextra)
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# The FlexRadio API's parsing (components/flex_client/flex_parse.c): pure C,
# taken here as the knob takes it -- discovery packets and memory statuses
# come off the network. radio.h's list helper beside it, with esp_err.h's
# few names from stub/.
set(FLEX ${CMAKE_CURRENT_SOURCE_DIR}/../../components/flex_client)
add_library(flex_parse STATIC ${FLEX}/flex_parse.c)
target_include_directories(flex_parse PUBLIC ${FLEX})
target_compile_options(flex_parse PRIVATE -Wall -Wextra -Wpedantic -Werror)
foreach(t test_flex_parse test_radio_list)
    add_executable(${t} src/${t}.c)
    target_link_libraries(${t} PRIVATE flex_parse)
    target_include_directories(${t} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../components/radio/include
        ${CMAKE_CURRENT_SOURCE_DIR}/stub)
    target_compile_options(${t} PRIVATE -Wall -Wextra)
    add_test(NAME ${t} COMMAND ${t})
endforeach()

# The settings' copies on the SD card and the merge of NVS into them
# (components/kvstore's kv_tlv.c and kv_merge.c): pure C, taken here as the
# knob takes it -- a copy is read off a card that may have lost power mid-write.
set(KVS ${CMAKE_CURRENT_SOURCE_DIR}/../../components/kvstore)
add_library(kv_pure STATIC ${KVS}/kv_tlv.c ${KVS}/kv_merge.c)
target_include_directories(kv_pure PUBLIC ${KVS})
target_compile_options(kv_pure PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(test_kv src/test_kv.c)
target_link_libraries(test_kv PRIVATE kv_pure)
target_compile_options(test_kv PRIVATE -Wall -Wextra)
add_test(NAME test_kv COMMAND test_kv)

# The second chip's reading of a device's battery (companion/main/batt.c):
# pure C, taken here as the chip takes it -- the AT commands a headset or a
# speaker sends come off the air.
set(COMPANION ${CMAKE_CURRENT_SOURCE_DIR}/../../companion/main)
add_library(batt STATIC ${COMPANION}/batt.c)
target_include_directories(batt PUBLIC ${COMPANION})
target_compile_options(batt PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(test_batt_at src/test_batt_at.c)
target_link_libraries(test_batt_at PRIVATE batt)
target_compile_options(test_batt_at PRIVATE -Wall -Wextra)
add_test(NAME test_batt_at COMMAND test_batt_at)

# The knob's own battery, from its 5 V rail (components/board/knob_batt.c):
# pure C, fed here the readings the battery knob gave, and a run-down.
set(BOARD ${CMAKE_CURRENT_SOURCE_DIR}/../../components/board)
add_library(knob_batt STATIC ${BOARD}/knob_batt.c)
target_include_directories(knob_batt PUBLIC ${BOARD}/include)
target_compile_options(knob_batt PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(test_knob_batt src/test_knob_batt.c)
target_link_libraries(test_knob_batt PRIVATE knob_batt m)
target_compile_options(test_knob_batt PRIVATE -Wall -Wextra)
add_test(NAME test_knob_batt COMMAND test_knob_batt)

# The mix a Bluetooth headset or speaker takes (components/audio/mix_tap.h),
# before the volume: a block past full scale brought down whole, not clipped.
add_executable(test_mix_tap src/test_mix_tap.c)
target_include_directories(test_mix_tap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../components/audio)
target_link_libraries(test_mix_tap PRIVATE m)
target_compile_options(test_mix_tap PRIVATE -Wall -Wextra)
add_test(NAME test_mix_tap COMMAND test_mix_tap)

# A Bluetooth headset's or speaker's level (components/bt_link/bt_level.c):
# pure C -- its steps and gains, the defaults, the gain on top of the VOLUME,
# the gain turned down smoothly where a loud passage would pass full scale,
# and the devices it is kept for -- fed here the tap's own sums from before
# levels to match.
set(BTLINK ${CMAKE_CURRENT_SOURCE_DIR}/../../components/bt_link)
add_library(bt_level STATIC ${BTLINK}/bt_level.c)
target_include_directories(bt_level PUBLIC ${BTLINK}/include)
target_compile_options(bt_level PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(test_bt_level src/test_bt_level.c)
target_link_libraries(test_bt_level PRIVATE bt_level m)
target_compile_options(test_bt_level PRIVATE -Wall -Wextra)
add_test(NAME test_bt_level COMMAND test_bt_level)

# KiwiSDR's protocol, the plain-C part (components/kiwi_proto/kiwi_proto.c).
set(KIWI ${CMAKE_CURRENT_SOURCE_DIR}/../../components/kiwi_proto)
add_library(kiwi_proto STATIC ${KIWI}/kiwi_proto.c)
target_include_directories(kiwi_proto PUBLIC ${KIWI}/include)
target_link_libraries(kiwi_proto PUBLIC m)          # the resampler's filters are designed at its rate
target_compile_options(kiwi_proto PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(test_kiwi_proto src/test_kiwi_proto.c)
target_link_libraries(test_kiwi_proto PRIVATE kiwi_proto m)
target_compile_options(test_kiwi_proto PRIVATE -Wall -Wextra)
add_test(NAME test_kiwi_proto COMMAND test_kiwi_proto)
# ...and the same fed a million rounds of what any address on the internet
# might send: random bytes, mutated messages, every truncation.
add_executable(fuzz_kiwi src/fuzz_kiwi.c)
target_link_libraries(fuzz_kiwi PRIVATE kiwi_proto)
target_compile_options(fuzz_kiwi PRIVATE -Wall -Wextra)
add_test(NAME fuzz_kiwi COMMAND fuzz_kiwi)
set_tests_properties(fuzz_kiwi PROPERTIES TIMEOUT 600 ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")

# OpenWebRX's protocol, the plain-C part (components/owrx_proto): its
# messages read in pieces broken at every byte, status.json and its join, the
# writers, the modes, the audio decoder...
set(OWRX ${CMAKE_CURRENT_SOURCE_DIR}/../../components/owrx_proto)
add_library(owrx_proto STATIC ${OWRX}/owrx_proto.c ${OWRX}/owrx_adpcm.c)
target_include_directories(owrx_proto PUBLIC ${OWRX}/include)
target_link_libraries(owrx_proto PUBLIC m)
target_compile_options(owrx_proto PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(test_owrx_proto src/test_owrx_proto.c)
target_link_libraries(test_owrx_proto PRIVATE owrx_proto m)
target_compile_options(test_owrx_proto PRIVATE -Wall -Wextra)
add_test(NAME test_owrx_proto COMMAND test_owrx_proto)
# ...the decoder against csdr's encoder, as tools/owrx_adpcm.py ports it
# (its check-csdr held it to csdr's own, byte for byte): 12 and 48 kHz, the
# stream in pieces of every size...
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    foreach(_rate 12000 48000)
        add_test(NAME owrx_adpcm_${_rate}
                 COMMAND sh -c "\"$1\" -I \"$2\" vectors \"$3.bin\" \"$3.pcm\" --rate ${_rate} && \"$4\" \"$3.bin\" \"$3.pcm\""
                         sh ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/owrx_adpcm.py
                         ${CMAKE_CURRENT_BINARY_DIR}/owrx_vectors_${_rate} $<TARGET_FILE:test_owrx_proto>)
    endforeach()
    # ...and as OpenWebRX 1.0 and 1.1 send it, without SYNCs...
    add_test(NAME owrx_adpcm_plain
             COMMAND sh -c "\"$1\" -I \"$2\" vectors \"$3.bin\" \"$3.pcm\" --plain && \"$4\" \"$3.bin\" \"$3.pcm\""
                     sh ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/owrx_adpcm.py
                     ${CMAKE_CURRENT_BINARY_DIR}/owrx_vectors_plain $<TARGET_FILE:test_owrx_proto>)
endif()
# ...the mock receiver (tools/mock_owrx.py) tried by a client shaped as the
# receivers' own page, through every case the knob must meet...
if(Python3_FOUND)
    add_test(NAME owrx_mock
             COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/owrx_mock.py
                     --mock ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/mock_owrx.py)
    set_tests_properties(owrx_mock PROPERTIES TIMEOUT 600 LABELS mock)
endif()
# ...and the same fed a million rounds of what any address might send.
# By hand, for as long as it is let run: fuzz_owrx 0 [SEED].
add_executable(fuzz_owrx src/fuzz_owrx.c)
target_link_libraries(fuzz_owrx PRIVATE owrx_proto)
target_compile_options(fuzz_owrx PRIVATE -Wall -Wextra)
add_test(NAME fuzz_owrx COMMAND fuzz_owrx)
set_tests_properties(fuzz_owrx PROPERTIES TIMEOUT 900 ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")

# PA3FWM's WebSDR protocol, the plain-C part (components/wsdr_proto): its
# audio items against tools/wsdr_codec.py's encoder sample for sample,
# bandinfo.js read in pieces broken at every byte, the stream path, the
# tuning it is sent...
set(WSDR ${CMAKE_CURRENT_SOURCE_DIR}/../../components/wsdr_proto)
add_library(wsdr_proto STATIC ${WSDR}/wsdr_proto.c ${WSDR}/wsdr_codec.c)
target_include_directories(wsdr_proto PUBLIC ${WSDR}/include)
target_link_libraries(wsdr_proto PUBLIC m)
target_compile_options(wsdr_proto PRIVATE -Wall -Wextra -Wpedantic -Werror)
add_executable(test_wsdr_proto src/test_wsdr_proto.c)
target_link_libraries(test_wsdr_proto PRIVATE wsdr_proto m)
target_compile_options(test_wsdr_proto PRIVATE -Wall -Wextra)
add_test(NAME test_wsdr_proto COMMAND test_wsdr_proto)
if(Python3_FOUND)
    foreach(_rate 7119 14238)
        add_test(NAME wsdr_codec_${_rate}
                 COMMAND sh -c "\"$1\" -I \"$2\" vectors \"$3.bin\" \"$3.pcm\" --rate ${_rate} && \"$4\" \"$3.bin\" \"$3.pcm\""
                         sh ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/wsdr_codec.py
                         ${CMAKE_CURRENT_BINARY_DIR}/wsdr_vectors_${_rate} $<TARGET_FILE:test_wsdr_proto>)
    endforeach()
    # ...the mock WebSDR (tools/mock_wsdr.py) tried by a client shaped as a
    # site's own page, through every case the knob must meet...
    add_test(NAME wsdr_mock
             COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/wsdr_mock.py
                     --mock ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/mock_wsdr.py)
    set_tests_properties(wsdr_mock PROPERTIES TIMEOUT 600 LABELS mock)
endif()
# ...and fed a million rounds of what any address might send.
add_executable(fuzz_wsdr src/fuzz_wsdr.c)
target_link_libraries(fuzz_wsdr PRIVATE wsdr_proto)
target_compile_options(fuzz_wsdr PRIVATE -Wall -Wextra)
add_test(NAME fuzz_wsdr COMMAND fuzz_wsdr)
# UBSan only prints and goes on unless told: an overflow fails the run.
set_tests_properties(fuzz_wsdr PROPERTIES TIMEOUT 900 ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")

# The web SDR beside a radio -- components/sdr_rx on the session in
# components/kiwi_proto, with its day-limit marks, as the firmware compiles
# them -- on the PC: shim/ stands in for ESP-IDF (NVS in a file, so a second
# run is a restart; audio_out's rings played by the PC's clock), and
# tools/mock_kiwi.py for the receiver. kiwi_limits.py boots it again and
# again and asks the mock how many logins it refused: never a real
# receiver's limits spent on a test.
set(SDR ${CMAKE_CURRENT_SOURCE_DIR}/../../components/sdr_rx)
find_package(Threads REQUIRED)
# Their TLS: kiwi_tls.c on mbedTLS, the certificate bundle the test's own CA
# (shim/shim_tls.c, KIWI_TEST_CA) -- or, with no mbedTLS, a stand-in that
# speaks none (shim/notls.c).
if(KIWI_TLS)
    set(KIWI_TLS_SRCS ${KIWI}/kiwi_tls.c shim/shim_tls.c)
    set(KIWI_TLS_LIBS mbedtls_host)
else()
    set(KIWI_TLS_SRCS shim/notls.c)
    set(KIWI_TLS_LIBS)
endif()
add_executable(sdr_host
    src/sdr_host.c
    shim/shim.c
    ${SDR}/sdr_rx.c
    ${KIWI}/kiwi_sess.c
    ${KIWI}/kiwi_mark.c
    ${KIWI_TLS_SRCS})
target_include_directories(sdr_host PRIVATE shim ${SDR}/include ${KIWI})
# As ESP-IDF builds; and RTC memory, which a crash leaves as it was, a file.
target_compile_definitions(sdr_host PRIVATE _GNU_SOURCE KIWI_RTC_SHIM)
target_link_libraries(sdr_host PRIVATE kiwi_proto ${KIWI_TLS_LIBS} Threads::Threads)
# The firmware's own warnings, as ESP-IDF sets them.
target_compile_options(sdr_host PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare)
# ...and the kiwi firmware's receiver: components/kiwi_client on the session
# in components/kiwi_proto, with sdr_rx's list, the same way.
set(KC ${CMAKE_CURRENT_SOURCE_DIR}/../../components/kiwi_client)
add_executable(kiwi_host
    src/kiwi_host.c
    shim/shim.c
    ${KC}/kiwi_client.c
    ${KIWI}/kiwi_sess.c
    ${KIWI}/kiwi_mark.c
    ${SDR}/sdr_rx.c
    ${KIWI_TLS_SRCS})
target_include_directories(kiwi_host PRIVATE shim ${KC}/include ${SDR}/include ${KIWI}
                           ${CMAKE_CURRENT_SOURCE_DIR}/../../components/radio/include)
target_compile_definitions(kiwi_host PRIVATE _GNU_SOURCE KIWI_RTC_SHIM)
target_link_libraries(kiwi_host PRIVATE kiwi_proto vfo_core ${KIWI_TLS_LIBS} Threads::Threads)
target_compile_options(kiwi_host PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare)

# OpenWebRX's session -- components/owrx_proto's owrx_sess.c on the web SDRs'
# link (components/websdr_link), as the firmware builds them -- on the PC
# against tools/mock_owrx.py, its audio into the shim's ring.
set(WSL ${CMAKE_CURRENT_SOURCE_DIR}/../../components/websdr_link)
add_executable(owrx_link_host
    src/owrx_link_host.c
    shim/shim.c
    ${WSL}/websdr_link.c
    ${WSL}/wl_flow.c
    ${OWRX}/owrx_sess.c
    ${KIWI_TLS_SRCS})
target_include_directories(owrx_link_host PRIVATE shim ${WSL}/include ${KIWI})
target_compile_definitions(owrx_link_host PRIVATE _GNU_SOURCE KIWI_RTC_SHIM)
target_link_libraries(owrx_link_host PRIVATE owrx_proto kiwi_proto ${KIWI_TLS_LIBS} Threads::Threads)
target_compile_options(owrx_link_host PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare)

# A WebSDR session -- components/wsdr_proto's wsdr_sess.c on the same link,
# as the firmware builds it -- on the PC against tools/mock_wsdr.py. Its
# addresses are read as OpenWebRX's are (owrx_url).
add_executable(wsdr_link_host
    src/wsdr_link_host.c
    shim/shim.c
    ${WSL}/websdr_link.c
    ${WSL}/wl_flow.c
    ${WSDR}/wsdr_sess.c
    ${KIWI_TLS_SRCS})
target_include_directories(wsdr_link_host PRIVATE shim ${WSL}/include ${KIWI})
target_compile_definitions(wsdr_link_host PRIVATE _GNU_SOURCE KIWI_RTC_SHIM)
target_link_libraries(wsdr_link_host PRIVATE wsdr_proto owrx_proto kiwi_proto ${KIWI_TLS_LIBS} Threads::Threads)
target_compile_options(wsdr_link_host PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare)

# ...and the openwebrx firmware's receiver: components/owrx_client on that
# session, with a radios' list of its own in the place of net_prov's.
set(OC ${CMAKE_CURRENT_SOURCE_DIR}/../../components/owrx_client)
add_executable(owrx_host
    src/owrx_host.c
    shim/shim.c
    ${OC}/owrx_client.c
    ${WSL}/websdr_link.c
    ${WSL}/wl_flow.c
    ${OWRX}/owrx_sess.c
    ${KIWI_TLS_SRCS})
target_include_directories(owrx_host PRIVATE shim ${OC}/include ${WSL}/include ${KIWI}
                           ${CMAKE_CURRENT_SOURCE_DIR}/../../components/net_prov/include
                           ${CMAKE_CURRENT_SOURCE_DIR}/../../components/radio/include)
target_compile_definitions(owrx_host PRIVATE _GNU_SOURCE KIWI_RTC_SHIM)
target_link_libraries(owrx_host PRIVATE owrx_proto kiwi_proto vfo_core ${KIWI_TLS_LIBS} Threads::Threads)
target_compile_options(owrx_host PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare)

# ...and the websdr firmware's receiver: components/wsdr_client on its
# session, with a radios' list of its own in the place of net_prov's.
set(WC ${CMAKE_CURRENT_SOURCE_DIR}/../../components/wsdr_client)
add_executable(wsdr_host
    src/wsdr_host.c
    shim/shim.c
    ${WC}/wsdr_client.c
    ${WSL}/websdr_link.c
    ${WSL}/wl_flow.c
    ${WSDR}/wsdr_sess.c
    ${KIWI_TLS_SRCS})
target_include_directories(wsdr_host PRIVATE shim ${WC}/include ${WSL}/include ${KIWI}
                           ${CMAKE_CURRENT_SOURCE_DIR}/../../components/net_prov/include
                           ${CMAKE_CURRENT_SOURCE_DIR}/../../components/radio/include)
target_compile_definitions(wsdr_host PRIVATE _GNU_SOURCE KIWI_RTC_SHIM)
target_link_libraries(wsdr_host PRIVATE wsdr_proto owrx_proto kiwi_proto vfo_core ${KIWI_TLS_LIBS} Threads::Threads)
target_compile_options(wsdr_host PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare)

# ...and with the knob's web server: components/webcfg as the openwebrx
# firmware compiles it, on shim/shim_httpd.c, with the knob's pages
# embedded as the firmware embeds them (a NUL after each) -- its configuration
# and radio pages driven in a browser against the mock (owrx_web.py). Needs
# mbedTLS for the page's login (base64) and webcfg's sha256.
if(KIWI_TLS)
    set(_www ${CMAKE_CURRENT_SOURCE_DIR}/../../components/webcfg/www)
    set(_pages ${CMAKE_CURRENT_BINARY_DIR}/web_pages.S)
    file(WRITE ${_pages} "")
    foreach(_p index radio portal)
        file(APPEND ${_pages} ".section .rodata\n.global _binary_${_p}_html_start\n.global _binary_${_p}_html_end\n"
                              "_binary_${_p}_html_start:\n.incbin \"${_www}/${_p}.html\"\n.byte 0\n"
                              "_binary_${_p}_html_end:\n")
    endforeach()
    file(APPEND ${_pages} ".section .note.GNU-stack,\"\",%progbits\n")
    set_source_files_properties(${_pages} PROPERTIES OBJECT_DEPENDS
                                "${_www}/index.html;${_www}/radio.html;${_www}/portal.html")
    enable_language(ASM)
    set(CMP ${CMAKE_CURRENT_SOURCE_DIR}/../../components)
    add_executable(owrx_web_host
        src/owrx_host.c
        src/web_stubs.c
        shim/shim.c
        shim/shim_httpd.c
        ${_pages}
        ${CMP}/webcfg/webcfg.c
        ${CMP}/radio/radio_found.c
        ${OC}/owrx_client.c
        ${WSL}/websdr_link.c
        ${WSL}/wl_flow.c
        ${OWRX}/owrx_sess.c
        ${KIWI_TLS_SRCS})
    target_include_directories(owrx_web_host PRIVATE shim ${OC}/include ${WSL}/include ${KIWI}
                               ${CMP}/net_prov/include ${CMP}/radio/include ${CMP}/webcfg/include
                               ${CMP}/ota/include ${CMP}/bt_link/include ${CMP}/ui/include ${CMP}/usb_net/include
                               ${CMP}/audio/include ${CMP}/board/include)
    target_compile_definitions(owrx_web_host PRIVATE _GNU_SOURCE KIWI_RTC_SHIM OWRX_WEB=1
                               VFO_RADIO_OWRX=1 VFO_RX_ONLY=1)
    target_link_libraries(owrx_web_host PRIVATE owrx_proto kiwi_proto vfo_core ${KIWI_TLS_LIBS} Threads::Threads)
    # The firmware's own flags leave format-truncation out, as the PC's gcc does not.
    target_compile_options(owrx_web_host PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
                           -Wno-format-truncation)
    # ...and the websdr firmware's, the same way (wsdr_web.py).
    add_executable(wsdr_web_host
        src/wsdr_host.c
        src/web_stubs.c
        shim/shim.c
        shim/shim_httpd.c
        ${_pages}
        ${CMP}/webcfg/webcfg.c
        ${CMP}/radio/radio_found.c
        ${WC}/wsdr_client.c
        ${WSL}/websdr_link.c
        ${WSL}/wl_flow.c
        ${WSDR}/wsdr_sess.c
        ${KIWI_TLS_SRCS})
    target_include_directories(wsdr_web_host PRIVATE shim ${WC}/include ${WSL}/include ${KIWI}
                               ${CMP}/net_prov/include ${CMP}/radio/include ${CMP}/webcfg/include
                               ${CMP}/ota/include ${CMP}/bt_link/include ${CMP}/ui/include ${CMP}/usb_net/include
                               ${CMP}/audio/include ${CMP}/board/include)
    target_compile_definitions(wsdr_web_host PRIVATE _GNU_SOURCE KIWI_RTC_SHIM WSDR_WEB=1
                               VFO_RADIO_WEBSDR=1 VFO_RX_ONLY=1)
    target_link_libraries(wsdr_web_host PRIVATE wsdr_proto owrx_proto kiwi_proto vfo_core ${KIWI_TLS_LIBS}
                          Threads::Threads)
    target_compile_options(wsdr_web_host PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare
                           -Wno-format-truncation)
endif()

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    # Without TLS in the harnesses, their scenarios about it are left out.
    if(KIWI_TLS)
        set(_no_tls)
    else()
        set(_no_tls --no-tls)
    endif()
    add_test(NAME kiwi_limits
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/kiwi_limits.py
                     --host $<TARGET_FILE:sdr_host>
                     --mock ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/mock_kiwi.py ${_no_tls})
    set_tests_properties(kiwi_limits PROPERTIES TIMEOUT 900 LABELS mock)
    add_test(NAME kiwi_client_limits
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/kiwi_client_limits.py
                     --host $<TARGET_FILE:kiwi_host>
                     --mock ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/mock_kiwi.py ${_no_tls})
    set_tests_properties(kiwi_client_limits PROPERTIES TIMEOUT 900 LABELS mock)
    # Its audio: a backlog in one jump, the drift followed (5 minutes here;
    # T2's hour with --minutes 60 by hand), the target grown while the stream
    # keeps breaking up and eased back once calm, the 30 s report, rej=, saves.
    add_test(NAME kiwi_audio
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/kiwi_audio.py
                     --host $<TARGET_FILE:kiwi_host>
                     --mock ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/mock_kiwi.py)
    set_tests_properties(kiwi_audio PROPERTIES TIMEOUT 900 LABELS mock)
    # ...and its two ears: a second receiver in the right one, from the same
    # list, on the same dial -- never both ears on one receiver; and each ear
    # on an UberSDR's Kiwi input (tools/mock_ubersdr.py).
    add_test(NAME kiwi_two_ears
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/kiwi_two_ears.py
                     --host $<TARGET_FILE:kiwi_host>
                     --mock ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/mock_kiwi.py
                     --uber ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/mock_ubersdr.py ${_no_tls})
    set_tests_properties(kiwi_two_ears PROPERTIES TIMEOUT 600 LABELS mock)
    # OpenWebRX's session against its mock: a minute's stream (T2's ten
    # minutes with --minutes 10 by hand), TLS behind a redirect, a squelch's
    # pauses, band changes, a stall, WFM, and every end.
    add_test(NAME owrx_link
             COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/owrx_link.py
                     --host $<TARGET_FILE:owrx_link_host>
                     --mock ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/mock_owrx.py ${_no_tls})
    set_tests_properties(owrx_link PROPERTIES TIMEOUT 900 LABELS mock)
    # A WebSDR session against its mock: Twente's shape a minute (--minutes
    # 10 by hand), Maasbree's bands and ?v=11, the dial turned fast, AM's
    # doubled rate, mute and the squelch, the idle timeout, a stall, TLS in
    # front, and every refusal.
    add_test(NAME wsdr_link
             COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/wsdr_link.py
                     --host $<TARGET_FILE:wsdr_link_host>
                     --mock ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/mock_wsdr.py ${_no_tls})
    set_tests_properties(wsdr_link PROPERTIES TIMEOUT 900 LABELS mock)
    # ...and the openwebrx firmware's receiver on it: its bands, the dial at
    # their edges, another band no sooner than 11 s after the last, another
    # listener's followed; the receivers handed over, one full or banned
    # waited for as its owner asks, another chosen live; what is kept
    # through a restart.
    add_test(NAME owrx_client
             COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/owrx_client.py
                     --host $<TARGET_FILE:owrx_host>
                     --mock ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/mock_owrx.py ${_no_tls})
    set_tests_properties(owrx_client PROPERTIES TIMEOUT 900 LABELS mock)
    # ...and the websdr firmware's receiver on its session: where a site
    # starts it, its bands and band plan, the dial over their gaps, hand-over,
    # busy, the idle timeout, mute, a restart, the Test.
    add_test(NAME wsdr_client
             COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/wsdr_client.py
                     --host $<TARGET_FILE:wsdr_host>
                     --mock ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/mock_wsdr.py)
    set_tests_properties(wsdr_client PROPERTIES TIMEOUT 900 LABELS mock)
    # ...and its pages, in headless Chromium: the receivers' list, Test,
    # Save and In use; the radio page's bands, mode, passband, squelch and
    # receivers -- what each click sends the receiver. Skipped (77) without
    # a Chromium.
    if(TARGET owrx_web_host)
        add_test(NAME owrx_web
                 COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/owrx_web.py
                         --host $<TARGET_FILE:owrx_web_host>
                         --mock ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/mock_owrx.py)
        set_tests_properties(owrx_web PROPERTIES TIMEOUT 600 LABELS mock SKIP_RETURN_CODE 77)
    endif()
    # ...and the websdr firmware's pages the same way: the receivers' list,
    # Test, the listener name; the radio page's bands or band plan, the
    # meter, mode, passband and squelch, the receivers.
    if(TARGET wsdr_web_host)
        add_test(NAME wsdr_web
                 COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/wsdr_web.py
                         --host $<TARGET_FILE:wsdr_web_host>
                         --mock ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/mock_wsdr.py)
        set_tests_properties(wsdr_web PROPERTIES TIMEOUT 600 LABELS mock SKIP_RETURN_CODE 77)
    endif()
    # ...and the three again with every receiver behind TLS, as one behind
    # the kiwisdr.com proxy or Cloudflare is reached: the owners' limits as
    # they are in the clear. A run that cannot be had (no openssl to make
    # the run's certificates) is skipped.
    if(KIWI_TLS)
        foreach(_t kiwi_limits kiwi_client_limits kiwi_two_ears)
            set(_bin $<TARGET_FILE:kiwi_host>)
            if(_t STREQUAL "kiwi_limits")
                set(_bin $<TARGET_FILE:sdr_host>)
            endif()
            add_test(NAME ${_t}_tls
                     COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/${_t}.py --tls
                             --host ${_bin}
                             --mock ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/mock_kiwi.py)
            set_tests_properties(${_t}_tls PROPERTIES TIMEOUT 900 LABELS "mock;tls" SKIP_RETURN_CODE 77)
        endforeach()
    endif()
endif()
