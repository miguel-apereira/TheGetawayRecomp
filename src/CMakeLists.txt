# SPDX-License-Identifier: MIT

# tgr_core: platform-independent code shared by the game and the tools.
add_library(tgr_core STATIC
    app/Log.cpp
    disc/Crc32.cpp
    disc/GameData.cpp
    disc/GameDisc.cpp
    disc/IsoImage.cpp
    disc/PakArchive.cpp
    disc/SystemCnf.cpp
)
target_include_directories(tgr_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
tgr_set_project_options(tgr_core)

# The game executable.
add_executable(TheGetawayRecomp
    main.cpp
    host/VulkanRenderer.cpp
)
target_link_libraries(TheGetawayRecomp PRIVATE tgr_core SDL3::SDL3-static volk)
tgr_set_project_options(TheGetawayRecomp)

if(TGR_WITH_RECOMPILED_CODE)
    target_sources(TheGetawayRecomp PRIVATE game/RecompiledCode.cpp game/Pfs.cpp game/Stream.cpp)
    target_link_libraries(TheGetawayRecomp PRIVATE tgr_recompiled)
    target_compile_definitions(TheGetawayRecomp PRIVATE TGR_WITH_RECOMPILED_CODE=1)
endif()
