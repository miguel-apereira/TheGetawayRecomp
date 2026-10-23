# SPDX-License-Identifier: MIT
add_executable(tgr-profile main.cpp)
target_link_libraries(tgr-profile PRIVATE dbghelp winmm)
tgr_set_project_options(tgr-profile)
