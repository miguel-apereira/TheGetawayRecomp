# SPDX-License-Identifier: MIT
add_executable(tgr-disc main.cpp)
target_link_libraries(tgr-disc PRIVATE tgr_core)
tgr_set_project_options(tgr-disc)
