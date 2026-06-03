idf_component_register(SRCS "gateway.c"
                        INCLUDE_DIRS "."
                        REQUIRES esp_wifi nvs_flash zenoh_pico_idf zenoh_espnow)
