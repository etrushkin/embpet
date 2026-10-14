# Warning policy for project components; the host unit tests (test/host) use the same set,
# so target and host builds must agree.
#
# Call embpet_component_warnings() from a component's CMakeLists.txt after
# idf_component_register(). Header-only components are INTERFACE libraries, which cannot
# take PRIVATE options; there the flags propagate to the components that require them.
# Once such a component gains SRCS the function switches to PRIVATE by itself.
#
# The flags are appended after IDF's own -Wno-* flags so that they win.
#
# -Wunused-parameter is deliberately absent: IDF's own inline headers (e.g. spinlock.h via
# freertos/FreeRTOS.h) trip it, and IDF include dirs are not -isystem. Target builds keep
# IDF's -Wno-unused-parameter; the host build has no -Wno-* flags, so -Wextra still enables
# it there and the pure *_core.c files are checked for it.
set(EMBPET_WARNING_FLAGS
    -Wall -Wextra -Werror
    -Wsign-compare
    -Werror=unused-variable -Werror=unused-function -Werror=unused-but-set-variable)

function(embpet_component_warnings)
    get_target_property(_type ${COMPONENT_LIB} TYPE)
    if(_type STREQUAL "INTERFACE_LIBRARY")
        set(_scope INTERFACE)
    else()
        set(_scope PRIVATE)
    endif()
    target_compile_options(${COMPONENT_LIB} ${_scope} ${EMBPET_WARNING_FLAGS})
endfunction()
