idf_component_register(SRCS "pending_core.c" "prefix_core.c"
                       INCLUDE_DIRS include)

# Table size comes from Kconfig; pending_core.h falls back to 64 on the host.
target_compile_definitions(${COMPONENT_LIB} PUBLIC EMBPET_PENDING_MAX=${CONFIG_EMBPET_PENDING_MAX})

embpet_component_warnings()
