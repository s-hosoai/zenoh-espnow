idf_component_register(SRCS "espnow_node.c"
                        INCLUDE_DIRS "."
                        REQUIRES esp_wifi nvs_flash zenoh_pico_idf zenoh_espnow)
