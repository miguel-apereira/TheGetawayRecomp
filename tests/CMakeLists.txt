# SPDX-License-Identifier: MIT

# GS frontend tests on hand-written GIF packets, then a synthetic GS dump
# replayed through tgr-gsreplay (PS2Recomp's software GS).
add_executable(tgr-gstests gs/GsTests.cpp)
target_link_libraries(tgr-gstests PRIVATE tgr_gs)
tgr_set_project_options(tgr-gstests)

add_test(NAME gs.frontend COMMAND tgr-gstests)
add_test(NAME gs.write-dump COMMAND tgr-gstests --write-dump "${CMAKE_CURRENT_BINARY_DIR}/synthetic.tgrgs")
add_test(NAME gs.replay
    COMMAND tgr-gsreplay "${CMAKE_CURRENT_BINARY_DIR}/synthetic.tgrgs"
        --images "${CMAKE_CURRENT_BINARY_DIR}/synthetic-images" --inventory -)
set_tests_properties(gs.replay PROPERTIES DEPENDS gs.write-dump PASS_REGULAR_EXPRESSION "colour CT32 read as CT32")
