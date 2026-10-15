idf_component_register(SRCS "main.c" "cmd_sys.c"
                       PRIV_REQUIRES log console esp_app_format esp_timer esp_hw_support
                                     esp_system proto netcfg tgbot io)

embpet_component_warnings()
