set(RECOMP_ROM "${CMAKE_SOURCE_DIR}/local/hexen.us.z64")
set(RECOMP_SAVE "${CMAKE_SOURCE_DIR}/local/saves/hexen-us.mpk")
set(RECOMP_GAME_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/hexen.c")
include("${CMAKE_SOURCE_DIR}/cmake/Game.cmake")
