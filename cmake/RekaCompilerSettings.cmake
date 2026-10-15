# Shared warning / optimisation settings for every REKA target.

add_library(reka_compiler_settings INTERFACE)
add_library(reka::compiler_settings ALIAS reka_compiler_settings)

if(MSVC)
    target_compile_options(reka_compiler_settings INTERFACE
        /W4 /permissive- /Zc:__cplusplus /utf-8 /MP
        /wd4324) # structure padded due to alignment specifier (intentional for cache lines)
    target_compile_definitions(reka_compiler_settings INTERFACE
        NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS UNICODE _UNICODE)
    if(REKA_WARNINGS_AS_ERRORS)
        target_compile_options(reka_compiler_settings INTERFACE /WX)
    endif()
else()
    target_compile_options(reka_compiler_settings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wcast-align
        -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion)
    if(WIN32)
        target_compile_definitions(reka_compiler_settings INTERFACE
            NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    endif()
    if(REKA_WARNINGS_AS_ERRORS)
        target_compile_options(reka_compiler_settings INTERFACE -Werror)
    endif()
endif()
