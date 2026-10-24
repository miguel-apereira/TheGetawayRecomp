# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.25)

project(TheGetawayRecomp
    VERSION 0.1.0
    DESCRIPTION "Native PC static recompilation of The Getaway (PS2, SCUS-97133)"
    LANGUAGES C CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Build out of tree, for example with: cmake --preset msvc-debug")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Link the C/C++ runtime statically, so bin/ can be copied to another PC and
# run there without installing the Visual C++ redistributable (Debug included).
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# The game executable builds without any recompiled code until the
# pipeline (extract -> Ghidra -> ps2_recomp) has produced recomp/generated/.
option(TGR_WITH_RECOMPILED_CODE "Compile recomp/generated and link PS2Recomp's runtime" OFF)
option(TGR_BUILD_RECOMPILER "Build PS2Recomp's ps2_recomp tool" ON)
option(TGR_WARNINGS_AS_ERRORS "Treat warnings in our own code as errors" OFF)
option(TGR_BUILD_TESTS "Build the tests (run them with ctest)" ON)

include(TgrCompilerOptions)
include(TgrDependencies)

add_subdirectory(src)
add_subdirectory(tools/disc)
add_subdirectory(tools/gsreplay)
add_subdirectory(tools/vurecomp)
if(WIN32)
    add_subdirectory(tools/profile)
endif()
add_subdirectory(recomp)

if(TGR_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
