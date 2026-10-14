set(srcs "proto_core.c" "proto.c" "state.c")
if(CONFIG_EMBPET_PROTO_STRESS)
    list(APPEND srcs "proto_stress.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS include
                       REQUIRES json
                       PRIV_REQUIRES esp_driver_usb_serial_jtag esp_app_format esp_system)

# Ring size and byte cap come from Kconfig; proto_core.h falls back to 32 / 16384 on the host.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    EMBPET_EVT_RING=${CONFIG_EMBPET_EVT_RING}
    EMBPET_EVT_RING_BYTES=${CONFIG_EMBPET_EVT_RING_BYTES})

embpet_component_warnings()
