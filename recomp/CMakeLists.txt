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
# A batch holds the functions of one 2 KiB block of guest code (files are named
# <name>_0x<address>.cpp), so adding or removing a function only changes its
# own batch. Fixed-size batches would shift every batch after it instead.
# The function table (one statement per entry point) is big enough on its own.
set_target_properties(tgr_recompiled PROPERTIES UNITY_BUILD ON UNITY_BUILD_MODE GROUP)
foreach(source IN LISTS TGR_GENERATED_SOURCES)
    get_filename_component(name "${source}" NAME_WE)
    if(name STREQUAL "register_functions")
        continue()
    elseif(name MATCHES "_0x([0-9a-fA-F]+)$")
        math(EXPR block "0x${CMAKE_MATCH_1} >> 11" OUTPUT_FORMAT HEXADECIMAL)
        set(group "code_${block}")
    else()
        set(group "other") # not named <name>_0x<address>: not expected, but kept building
    endif()
    set_property(SOURCE "${source}" PROPERTY UNITY_GROUP "${group}")
endforeach()
set_source_files_properties("${TGR_GENERATED_DIR}/register_functions.cpp" PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)

# Every generated file includes the same instruction macros: parse them once.
# Only the slim guest headers go in here. A header in the precompiled header is
# part of every file, so a change to it rebuilds everything: ps2_runtime.h
# changes whenever a host takes over some hardware, and
# ps2_recompiled_functions.h whenever a function is added or renamed. With
# incremental_output in the ps2_recomp config, generated files include neither
# and declare what they call themselves (register_functions.cpp, the one file
# that needs the runtime class, includes it on its own).
target_precompile_headers(tgr_recompiled PRIVATE
    "$<TARGET_PROPERTY:ps2_runtime,SOURCE_DIR>/include/ps2_guest_macros.h"
)
if(MSVC)
    target_compile_options(tgr_recompiled PRIVATE /bigobj /W0)
else()
    target_compile_options(tgr_recompiled PRIVATE -w)
endif()

# tgr_vu_programs: the game's VU microprograms, compiled to C++ by tgr-vurecomp
# (recomp/generated/vu). They run on the model in src/hw/vu/VuCore.h.
file(GLOB TGR_VU_SOURCES CONFIGURE_DEPENDS "${TGR_GENERATED_DIR}/vu/*.cpp")
if(NOT TGR_VU_SOURCES)
    # tgr-vurecomp has not run yet (it is built with the rest): no programs.
    message(WARNING "recomp/generated/vu/ is empty: VU microprograms will not run. Run tools/scripts/recompile.ps1.")
    set(TGR_VU_SOURCES "${CMAKE_CURRENT_BINARY_DIR}/vu_programs_none.cpp")
    file(WRITE "${TGR_VU_SOURCES}"
        "#include \"hw/vu/VuPrograms.h\"\n"
        "namespace tgr::hw::vu\n{\n"
        "const ProgramInfo kPrograms[] = {{0, 0, 0, 0, 0, nullptr}};\n"
        "const std::size_t kProgramCount = 0;\n"
        "}\n")
endif()
add_library(tgr_vu_programs STATIC ${TGR_VU_SOURCES})
target_include_directories(tgr_vu_programs PRIVATE "${PROJECT_SOURCE_DIR}/src")
if(MSVC)
    target_compile_options(tgr_vu_programs PRIVATE /bigobj /W0)
else()
    target_compile_options(tgr_vu_programs PRIVATE -w)
endif()
