idf_component_register(SRCS "zenoh_pubsub.c"
                        INCLUDE_DIRS "."
                        REQUIRES esp_wifi nvs_flash zenoh_pico_idf)
