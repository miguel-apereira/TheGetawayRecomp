# SPDX-License-Identifier: MIT

# PS2Recomp's software GS on its own (no runtime, no window), as the
# reference renderer for replayed GS dumps.
tgr_require_submodule(PS2Recomp)
set(TGR_PS2X_RUNTIME "${TGR_EXTERNAL_DIR}/PS2Recomp/ps2xRuntime")
add_library(tgr_ps2recomp_gs STATIC
    "${TGR_PS2X_RUNTIME}/src/lib/gs/gs_cpu_backend.cpp"
    "${TGR_PS2X_RUNTIME}/src/lib/gs/gs_frontend.cpp"
    "${TGR_PS2X_RUNTIME}/src/lib/gs/ps2_gs_memory.cpp"
)
target_include_directories(tgr_ps2recomp_gs PUBLIC "${TGR_PS2X_RUNTIME}/include")
if(WIN32)
    target_compile_definitions(tgr_ps2recomp_gs PRIVATE NOMINMAX)
endif()

add_executable(tgr-gsreplay main.cpp)
target_link_libraries(tgr-gsreplay PRIVATE tgr_gs tgr_ps2recomp_gs)
tgr_set_project_options(tgr-gsreplay)
