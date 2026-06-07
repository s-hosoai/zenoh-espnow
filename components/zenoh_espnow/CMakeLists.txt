cmake_minimum_required(VERSION 3.16)

# zenoh_espnow exposes its public header but compiles its implementation
# source directly into zenoh_pico_idf.  This avoids a static-library
# ordering problem: zenoh_pico_idf's multicast link code calls _z_udp_multicast_*,
# and the linker must find those symbols in the *same* archive.
idf_component_register(
    INCLUDE_DIRS "include"
    REQUIRES zenoh_pico_idf esp_wifi
)

idf_component_get_property(zenoh_pico_lib zenoh_pico_idf COMPONENT_LIB)

# Inject implementation source into zenoh_pico_idf so symbols are co-located
target_sources(${zenoh_pico_lib} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/zenoh_espnow_link.c"
)
# Make zenoh_espnow.h visible when compiling zenoh_espnow_link.c
target_include_directories(${zenoh_pico_lib} PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
