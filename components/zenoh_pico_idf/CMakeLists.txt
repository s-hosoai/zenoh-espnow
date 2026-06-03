# ESP-IDF component wrapper for zenoh-pico
# Collects zenoh-pico sources and registers them as an ESP-IDF component.
# The upstream submodule is left unmodified; platform selection is done here
# by defining ZENOH_ESPIDF and adding src/system/espidf/*.c.

cmake_minimum_required(VERSION 3.16)

set(ZENOH_PICO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../zenoh-pico")

file(GLOB_RECURSE ZENOH_SOURCES
    "${ZENOH_PICO_DIR}/src/api/*.c"
    "${ZENOH_PICO_DIR}/src/collections/*.c"
    "${ZENOH_PICO_DIR}/src/link/*.c"
    "${ZENOH_PICO_DIR}/src/net/*.c"
    "${ZENOH_PICO_DIR}/src/protocol/*.c"
    "${ZENOH_PICO_DIR}/src/runtime/*.c"
    "${ZENOH_PICO_DIR}/src/session/*.c"
    "${ZENOH_PICO_DIR}/src/transport/*.c"
    "${ZENOH_PICO_DIR}/src/utils/*.c"
    "${ZENOH_PICO_DIR}/src/system/common/*.c"
    "${ZENOH_PICO_DIR}/src/system/espidf/*.c"
)

idf_component_register(
    SRCS ${ZENOH_SOURCES}
    INCLUDE_DIRS "${ZENOH_PICO_DIR}/include"
    REQUIRES esp_wifi nvs_flash lwip
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC ZENOH_ESPIDF)
