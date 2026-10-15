idf_component_register(SRCS "cfg_core.c" "cfg.c" "wifi_core.c" "wifi.c"
                       INCLUDE_DIRS include
                       REQUIRES esp_common
                       PRIV_REQUIRES nvs_flash log freertos esp_wifi esp_netif esp_event
                                     esp_timer mbedtls proto)

embpet_component_warnings()
