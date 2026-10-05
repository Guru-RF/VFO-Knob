# Host-side tests for components/vfo_core, the FlexRadio API's parsing, the
# second chip's reading of a device's battery, and the knob's own battery.
# No ESP-IDF, no cross-compiler.
#
#   cmake -B build_host -S test/host
#   cmake --build build_host
#   ctest --test-dir build_host --output-on-failure

cmake_minimum_required(VERSION 3.16)
project(vfo_core_host_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

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
