# SPDX-License-Identifier: MIT
#
# tgr_recompiled: the C++ that ps2_recomp generated from the user's own
# SCUS_971.33. It is built locally from the user's disc and never committed.

if(NOT TGR_WITH_RECOMPILED_CODE)
    return()
endif()

set(TGR_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/generated")
file(GLOB TGR_GENERATED_SOURCES CONFIGURE_DEPENDS "${TGR_GENERATED_DIR}/*.cpp")
if(NOT TGR_GENERATED_SOURCES)
    message(FATAL_ERROR
        "TGR_WITH_RECOMPILED_CODE is ON but recomp/generated/ is empty. "
        "Run tools/scripts/recompile.ps1 first.")
endif()

add_library(tgr_recompiled STATIC ${TGR_GENERATED_SOURCES})
target_include_directories(tgr_recompiled PUBLIC "${TGR_GENERATED_DIR}")
target_link_libraries(tgr_recompiled PUBLIC ps2_runtime)

# Thousands of small generated files: batch them, and skip warnings we cannot fix.
# The function table (one statement per entry point) is big enough on its own.
set_target_properties(tgr_recompiled PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 32)
set_source_files_properties("${TGR_GENERATED_DIR}/register_functions.cpp" PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)

# Every generated file includes the same heavy runtime headers: parse them once.
target_precompile_headers(tgr_recompiled PRIVATE
    "$<TARGET_PROPERTY:ps2_runtime,SOURCE_DIR>/include/ps2_runtime_macros.h"
    "$<TARGET_PROPERTY:ps2_runtime,SOURCE_DIR>/include/ps2_runtime.h"
    "$<TARGET_PROPERTY:ps2_runtime,SOURCE_DIR>/include/ps2_syscalls.h"
    "$<TARGET_PROPERTY:ps2_runtime,SOURCE_DIR>/include/ps2_stubs.h"
    "${TGR_GENERATED_DIR}/ps2_recompiled_functions.h"
)
if(MSVC)
    target_compile_options(tgr_recompiled PRIVATE /bigobj /W0)
else()
    target_compile_options(tgr_recompiled PRIVATE -w)
endif()
