set(RECOMP_ROM "${CMAKE_SOURCE_DIR}/local/duke.us.z64")
set(RECOMP_SAVE "${CMAKE_SOURCE_DIR}/local/saves/duke-us.mpk")
set(RECOMP_GAME_SOURCES fov.c input.c)
include("${CMAKE_SOURCE_DIR}/cmake/Game.cmake")
