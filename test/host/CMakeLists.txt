# Host-side tests for components/vfo_core. No ESP-IDF, no cross-compiler.
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
