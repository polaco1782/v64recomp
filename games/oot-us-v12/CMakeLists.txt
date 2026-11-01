set(RECOMP_ROM "${CMAKE_SOURCE_DIR}/local/oot.us.v12.z64")
set(RECOMP_SAVE "${CMAKE_SOURCE_DIR}/local/oot.us.v12.z64.sram")
set(RECOMP_GAME_SOURCES controls.c)
set(RECOMP_GAME_DEBUG_SOURCES debug/controls_check.c)

include("${CMAKE_SOURCE_DIR}/cmake/Game.cmake")
target_link_libraries(${RECOMP_GAME} PRIVATE recomp_jpeg)
