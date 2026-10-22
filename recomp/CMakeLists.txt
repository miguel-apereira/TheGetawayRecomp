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
set_target_properties(tgr_recompiled PROPERTIES UNITY_BUILD ON UNITY_BUILD_BATCH_SIZE 32)
if(MSVC)
    target_compile_options(tgr_recompiled PRIVATE /bigobj /W0)
else()
    target_compile_options(tgr_recompiled PRIVATE -w)
endif()
