idf_component_register(SRCS "cfg_core.c" "cfg.c"
                       INCLUDE_DIRS include
                       REQUIRES esp_common
                       PRIV_REQUIRES nvs_flash log freertos)

embpet_component_warnings()
