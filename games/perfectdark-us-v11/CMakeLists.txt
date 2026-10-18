set(RECOMP_ROM "${CMAKE_SOURCE_DIR}/local/perfectdark.us.v11.z64")
set(RECOMP_SAVE "${CMAKE_SOURCE_DIR}/local/saves/perfectdark-us-v11.eeprom")
set(RECOMP_GAME_SOURCES perfectdark.c)
include("${CMAKE_SOURCE_DIR}/cmake/Game.cmake")
