if(NOT TARGET fmt::fmt-header-only)
    add_library(fmt::fmt-header-only INTERFACE IMPORTED)
    set_target_properties(fmt::fmt-header-only PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "/opt/prey-headers/include"
        INTERFACE_COMPILE_DEFINITIONS "FMT_HEADER_ONLY=1"
    )
endif()

set(fmt_FOUND TRUE)
