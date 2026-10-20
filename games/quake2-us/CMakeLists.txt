set(RECOMP_ROM "${CMAKE_SOURCE_DIR}/local/quake2.us.z64")
set(RECOMP_SAVE "${CMAKE_SOURCE_DIR}/local/saves/quake2-us.mpk")
set(RECOMP_GAME_SOURCES run.c)
include("${CMAKE_SOURCE_DIR}/cmake/Game.cmake")
