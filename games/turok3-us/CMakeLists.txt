set(RECOMP_ROM "${CMAKE_SOURCE_DIR}/local/turok3.us.z64")
set(RECOMP_SAVE "${CMAKE_SOURCE_DIR}/local/saves/turok3-us.mpk")
set(RECOMP_GAME_SOURCES
    "${CMAKE_SOURCE_DIR}/backends/acclaim_pager.c"
    "${CMAKE_SOURCE_DIR}/backends/acclaim_controls.c"
    "${CMAKE_SOURCE_DIR}/backends/acclaim_graphics.c"
    "${CMAKE_SOURCE_DIR}/backends/acclaim_lighting.c")
include("${CMAKE_SOURCE_DIR}/cmake/Game.cmake")
