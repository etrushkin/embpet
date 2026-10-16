set(srcs "proto_core.c" "proto.c" "state.c" "escape_core.c" "console.c")
if(CONFIG_EMBPET_PROTO_STRESS)
    list(APPEND srcs "proto_stress.c")
endif()
if(CONFIG_EMBPET_CONSOLE_SOAK)
    list(APPEND srcs "console_soak.c")
endif()
if(CONFIG_EMBPET_CONSOLE_PROBE)
    list(APPEND srcs "console_probe.c")
endif()
if(CONFIG_EMBPET_BOOT_TRACE)
    list(APPEND srcs "boot_trace.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS include
                       REQUIRES json esp_common
                       PRIV_REQUIRES console log esp_driver_usb_serial_jtag esp_app_format
                                     esp_system esp_timer hal mbedtls)

# Ring size and byte cap come from Kconfig; proto_core.h falls back to 32 / 16384 on the host.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    EMBPET_EVT_RING=${CONFIG_EMBPET_EVT_RING}
    EMBPET_EVT_RING_BYTES=${CONFIG_EMBPET_EVT_RING_BYTES})

if(CONFIG_EMBPET_BOOT_TRACE)
    # boot_trace.c times these calls (__wrap_*).
    foreach(sym register_chipv7_phy pbkdf2_sha1 esp_flash_erase_region esp_flash_write
                mbedtls_ssl_handshake esp_wifi_init esp_wifi_start esp_event_post
                sae_derive_pt sae_prepare_commit_pt sae_prepare_commit sae_process_commit)
        target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=${sym}")
    endforeach()
endif()

embpet_component_warnings()
