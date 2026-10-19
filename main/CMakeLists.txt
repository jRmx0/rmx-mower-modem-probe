idf_component_register(SRCS "app_main.c" "board.c" "cell.c" "mqtt_link.c" "https_api.c" "probe_proto.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_driver_gpio esp_netif esp_event esp_http_client esp_timer
                                     esp_hw_support mbedtls nvs_flash app_update)
