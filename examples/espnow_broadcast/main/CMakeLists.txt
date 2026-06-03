idf_component_register(SRCS "espnow_broadcast.c"
                        INCLUDE_DIRS "."
                        REQUIRES esp_wifi esp_now nvs_flash)
