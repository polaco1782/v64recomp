set(RECOMP_ROM "${CMAKE_SOURCE_DIR}/local/doom64.us.z64")
set(RECOMP_SAVE "${CMAKE_SOURCE_DIR}/local/saves/doom64-us.mpk")
set(RECOMP_GAME_SOURCES controls.c fov.c)
include("${CMAKE_SOURCE_DIR}/cmake/Game.cmake")
