idf_component_register(SRCS "ppp_smoke.c"
                       PRIV_REQUIRES esp_driver_gpio esp_netif esp_event esp_http_client esp_timer mbedtls nvs_flash)
