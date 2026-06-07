# ESP-IDF component wrapper for zenoh-pico
# Generates config.h from config.h.in and registers zenoh-pico sources as an ESP-IDF component.
# udp_multicast_esp32.c is excluded because zenoh_espnow provides the multicast functions.

cmake_minimum_required(VERSION 3.16)

set(ZENOH_PICO_DIR "${CMAKE_CURRENT_LIST_DIR}/../../zenoh-pico")

# --- Generate config.h from upstream config.h.in ---
# Values match upstream defaults; Z_BATCH_MULTICAST_SIZE is set to the ESP-NOW 250-byte frame limit.
set(FRAG_MAX_SIZE 4096)
set(BATCH_UNICAST_SIZE 2048)
set(BATCH_MULTICAST_SIZE 250)
set(Z_CONFIG_SOCKET_TIMEOUT 100)
set(Z_TRANSPORT_LEASE 10000)
set(Z_TRANSPORT_LEASE_EXPIRE_FACTOR 3)
set(Z_RUNTIME_MAX_TASKS 64)
set(Z_TRANSPORT_ACCEPT_TIMEOUT 1000)
set(Z_TRANSPORT_CONNECT_TIMEOUT 10000)

set(Z_FEATURE_CONNECTIVITY 0)
set(Z_FEATURE_MULTI_THREAD 1)
set(Z_FEATURE_PUBLICATION 1)
set(Z_FEATURE_ADVANCED_PUBLICATION 0)
set(Z_FEATURE_SUBSCRIPTION 1)
set(Z_FEATURE_ADVANCED_SUBSCRIPTION 0)
set(Z_FEATURE_QUERY 1)
set(Z_FEATURE_QUERYABLE 1)
set(Z_FEATURE_LIVELINESS 1)
set(Z_FEATURE_RAWETH_TRANSPORT 0)
set(Z_FEATURE_INTEREST 1)
set(Z_FEATURE_LINK_TCP 1)
set(Z_FEATURE_LINK_BLUETOOTH 0)
set(Z_FEATURE_LINK_WS 0)
set(Z_FEATURE_LINK_SERIAL 0)
set(Z_FEATURE_LINK_SERIAL_USB 0)
set(Z_FEATURE_LINK_TLS 0)
set(Z_FEATURE_SCOUTING 1)
set(Z_FEATURE_LINK_UDP_MULTICAST 1)
set(Z_FEATURE_LINK_UDP_UNICAST 1)
set(Z_FEATURE_MULTICAST_TRANSPORT 1)
set(Z_FEATURE_UNICAST_TRANSPORT 1)
set(Z_FEATURE_FRAGMENTATION 1)
set(Z_FEATURE_ENCODING_VALUES 1)
set(Z_FEATURE_TCP_NODELAY 1)
set(Z_FEATURE_LOCAL_SUBSCRIBER 0)
set(Z_FEATURE_LOCAL_QUERYABLE 0)
set(Z_FEATURE_SESSION_CHECK 1)
set(Z_FEATURE_BATCHING 1)
set(Z_FEATURE_BATCH_TX_MUTEX 0)
set(Z_FEATURE_BATCH_PEER_MUTEX 0)
set(Z_FEATURE_MATCHING 1)
set(Z_FEATURE_RX_CACHE 0)
set(Z_FEATURE_UNICAST_PEER 1)
set(Z_FEATURE_AUTO_RECONNECT 1)
set(Z_FEATURE_MULTICAST_DECLARATIONS 0)
set(Z_FEATURE_ADMIN_SPACE 0)

configure_file(
    "${ZENOH_PICO_DIR}/include/zenoh-pico/config.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/include/zenoh-pico/config.h"
    @ONLY
)

# --- Sources ---
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
    "${ZENOH_PICO_DIR}/src/system/socket/*.c"
)

# zenoh_espnow provides the UDP multicast functions (_z_udp_multicast_*)
list(FILTER ZENOH_SOURCES EXCLUDE REGEX ".*udp_multicast_esp32\\.c$")

idf_component_register(
    SRCS ${ZENOH_SOURCES}
    INCLUDE_DIRS
        "${ZENOH_PICO_DIR}/include"
        "${CMAKE_CURRENT_BINARY_DIR}/include"
    REQUIRES esp_wifi nvs_flash lwip driver
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC ZENOH_ESPIDF)
