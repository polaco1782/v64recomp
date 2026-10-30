set(RECOMP_ROM "${CMAKE_SOURCE_DIR}/local/twine.us.z64")
set(RECOMP_SAVE "${CMAKE_SOURCE_DIR}/local/saves/twine-us.eeprom")
set(RECOMP_GAME_SOURCES run.c)
include("${CMAKE_SOURCE_DIR}/cmake/Game.cmake")
