set(srcs "pulse_core.c" "debounce_core.c" "io_out.c" "io_in.c" "io_cmd.c")
if(CONFIG_EMBPET_IO_TEST)
    list(APPEND srcs "io_test.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS include
                       REQUIRES esp_common
                       PRIV_REQUIRES esp_driver_gpio esp_timer freertos console json
                                     netcfg proto)

embpet_component_warnings()
