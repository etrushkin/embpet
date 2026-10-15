set(srcs "proto_core.c" "proto.c" "state.c" "escape_core.c" "console.c")
if(CONFIG_EMBPET_PROTO_STRESS)
    list(APPEND srcs "proto_stress.c")
endif()
if(CONFIG_EMBPET_CONSOLE_SOAK)
    list(APPEND srcs "console_soak.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS include
                       REQUIRES json esp_common
                       PRIV_REQUIRES console log esp_driver_usb_serial_jtag esp_app_format
                                     esp_system esp_timer mbedtls)

# Ring size and byte cap come from Kconfig; proto_core.h falls back to 32 / 16384 on the host.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    EMBPET_EVT_RING=${CONFIG_EMBPET_EVT_RING}
    EMBPET_EVT_RING_BYTES=${CONFIG_EMBPET_EVT_RING_BYTES})

embpet_component_warnings()
