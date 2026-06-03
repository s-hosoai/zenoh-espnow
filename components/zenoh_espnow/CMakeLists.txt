cmake_minimum_required(VERSION 3.16)

# zenoh_espnow exposes its public header but compiles its implementation
# source directly into zenoh_pico_idf.  This avoids a static-library
# ordering problem: zenoh_pico_idf's udp.c calls _z_open_udp_multicast etc.,
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
# Override UDP multicast functions:
#   ZENOH_ESPNOW_LINK_OVERRIDE: suppresses originals in network.c
#   Z_BATCH_MULTICAST_SIZE=250: clamps batch size to ESP-NOW v1.0 limit
target_compile_definitions(${zenoh_pico_lib} PRIVATE
    ZENOH_ESPNOW_LINK_OVERRIDE=1
    Z_BATCH_MULTICAST_SIZE=250
)
