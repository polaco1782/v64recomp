set(RECOMP_ROM "${CMAKE_SOURCE_DIR}/local/turok2.us.z64")
set(RECOMP_SAVE "${CMAKE_SOURCE_DIR}/local/saves/turok2-us.mpk")
set(RECOMP_GAME_SOURCES pager.c graphics.c lighting.c)
include("${CMAKE_SOURCE_DIR}/cmake/Game.cmake")
