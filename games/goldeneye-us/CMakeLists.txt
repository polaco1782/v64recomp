set(RECOMP_ROM "${CMAKE_SOURCE_DIR}/local/goldeneye.us.z64")
set(RECOMP_SAVE "${CMAKE_SOURCE_DIR}/local/saves/goldeneye-us.eeprom")
set(RECOMP_GAME_SOURCES pager.c graphics.cpp input.c audio_dma.c)
include("${CMAKE_SOURCE_DIR}/cmake/Game.cmake")
