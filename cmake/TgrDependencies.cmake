# SPDX-License-Identifier: MIT
#
# Third-party code lives in external/ as git submodules pinned to exact
# commits (see external/README.md). Run tools/scripts/bootstrap.ps1 once
# after cloning to fetch them.

set(TGR_EXTERNAL_DIR "${CMAKE_SOURCE_DIR}/external")

function(tgr_require_submodule name)
    if(NOT EXISTS "${TGR_EXTERNAL_DIR}/${name}/CMakeLists.txt")
        message(FATAL_ERROR
            "external/${name} is missing. Run tools/scripts/bootstrap.ps1 "
            "(or: git submodule update --init --recursive) and configure again.")
    endif()
endfunction()

# --- SDL3: window, input, audio --------------------------------------------
tgr_require_submodule(SDL)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${TGR_EXTERNAL_DIR}/SDL" EXCLUDE_FROM_ALL)

# --- Vulkan: headers + volk loader (no Vulkan SDK needed to build) ---------
tgr_require_submodule(Vulkan-Headers)
add_subdirectory("${TGR_EXTERNAL_DIR}/Vulkan-Headers" EXCLUDE_FROM_ALL)

tgr_require_submodule(volk)
set(VOLK_PULL_IN_VULKAN OFF CACHE BOOL "" FORCE)
add_subdirectory("${TGR_EXTERNAL_DIR}/volk" EXCLUDE_FROM_ALL)
target_link_libraries(volk PUBLIC Vulkan::Headers)

# --- PS2Recomp ---------------------------------------------------------------
if(TGR_BUILD_RECOMPILER OR TGR_WITH_RECOMPILED_CODE)
    tgr_require_submodule(PS2Recomp)
endif()

# The ps2_recomp and ps2_analyzer tools expect to be a top-level CMake
# project, so they get their own build tree (always Release: they are host
# tools and much faster that way). The executables land next to ours in bin/.
# ps2_analyzer is only used for its SCE SDK symbol database, which names the
# Sony library functions in the stripped ELF (see analyze-ghidra.ps1).
if(TGR_BUILD_RECOMPILER)
    include(ExternalProject)
    ExternalProject_Add(ps2recomp_tool
        SOURCE_DIR "${TGR_EXTERNAL_DIR}/PS2Recomp"
        BINARY_DIR "${CMAKE_BINARY_DIR}/ps2recomp"
        CMAKE_ARGS
            -DCMAKE_BUILD_TYPE=Release
            -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
            -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
            -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=${CMAKE_RUNTIME_OUTPUT_DIRECTORY}
            -DPS2X_BUILD_RECOMP=ON
            -DPS2X_BUILD_RUNTIME=OFF
            -DPS2X_BUILD_ANALYZER=ON
            -DPS2X_BUILD_TEST=OFF
            -DPS2X_BUILD_STUDIO=OFF
        BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target ps2_recomp ps2_analyzer --config Release
        INSTALL_COMMAND ""
        BUILD_ALWAYS ON
        USES_TERMINAL_BUILD ON
    )
endif()

# The runtime (memory, R5900 context, kernel, IOP) is linked into the game
# once there is generated code to run.
if(TGR_WITH_RECOMPILED_CODE)
    set(PS2X_BUILD_RECOMP   OFF CACHE BOOL "" FORCE)
    set(PS2X_BUILD_RUNTIME  ON  CACHE BOOL "" FORCE)
    set(PS2X_BUILD_ANALYZER OFF CACHE BOOL "" FORCE)
    set(PS2X_BUILD_TEST     OFF CACHE BOOL "" FORCE)
    set(PS2X_BUILD_STUDIO   OFF CACHE BOOL "" FORCE)
    # No prebuilt FFmpeg DLLs (FMV gets our own IPU decoder), no ImGui debug
    # panel (it only builds into PS2Recomp's own runner), and no per-function
    # entry logging in every generated function.
    set(PS2X_ENABLE_FFMPEG          OFF CACHE BOOL "" FORCE)
    set(PS2X_ENABLE_DEBUG_UI        OFF CACHE BOOL "" FORCE)
    set(PS2X_ENABLE_AGRESSIVE_LOGS  OFF CACHE BOOL "" FORCE)
    add_subdirectory("${TGR_EXTERNAL_DIR}/PS2Recomp" EXCLUDE_FROM_ALL)
endif()
