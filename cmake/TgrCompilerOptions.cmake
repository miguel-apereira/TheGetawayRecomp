# SPDX-License-Identifier: MIT
#
# tgr_set_project_options(<target>) applies our warning level and platform
# defines. It is only used on our own targets, never on external/ code.

function(tgr_set_project_options target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
        if(TGR_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic
            -Wno-missing-field-initializers) # Vulkan structs are brace-initialised with only sType
        if(TGR_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()

    if(WIN32)
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
    endif()
endfunction()
