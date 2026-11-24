set(RECOMP_ROM "${CMAKE_SOURCE_DIR}/local/turok2.us.z64")
set(RECOMP_SAVE "${CMAKE_SOURCE_DIR}/local/saves/turok2-us.mpk")
set(RECOMP_GAME_SOURCES
    "acclaim_pager.c"
    "acclaim_controls.c")
include("${CMAKE_SOURCE_DIR}/cmake/Game.cmake")
