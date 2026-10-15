set(RECOMP_ROM "${CMAKE_SOURCE_DIR}/local/goldeneye.us.z64")
set(RECOMP_SAVE "${CMAKE_SOURCE_DIR}/local/saves/goldeneye-us.eeprom")
set(RECOMP_GAME_SOURCES release/pager.c release/graphics.cpp)
include("${CMAKE_SOURCE_DIR}/cmake/Game.cmake")
