set(srcs "pending_core.c" "prefix_core.c" "tgapi_core.c" "tgapi.c")
if(CONFIG_EMBPET_TG_TEST)
    list(APPEND srcs "tgapi_test.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS include
                       REQUIRES json log esp_common
                       PRIV_REQUIRES esp_http_client mbedtls freertos console esp_timer
                                     netcfg proto)

# Table size comes from Kconfig; pending_core.h falls back to 64 on the host.
target_compile_definitions(${COMPONENT_LIB} PUBLIC EMBPET_PENDING_MAX=${CONFIG_EMBPET_PENDING_MAX})

embpet_component_warnings()
