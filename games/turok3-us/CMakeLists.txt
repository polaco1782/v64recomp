set(RECOMP_ROM "${CMAKE_SOURCE_DIR}/local/turok3.us.z64")
set(RECOMP_SAVE "${CMAKE_SOURCE_DIR}/local/saves/turok3-us.mpk")
set(RECOMP_GAME_GLES2_SUPPORTED ON)
set(RECOMP_GAME_SOURCES
    "${CMAKE_SOURCE_DIR}/backends/acclaim_pager.c"
    "${CMAKE_SOURCE_DIR}/backends/acclaim_controls.c")
if(RECOMP_RENDERER STREQUAL "rt64")
    list(APPEND RECOMP_GAME_SOURCES
        "${CMAKE_SOURCE_DIR}/backends/acclaim_graphics.c"
        "${CMAKE_SOURCE_DIR}/backends/acclaim_lighting.c")
endif()
include("${CMAKE_SOURCE_DIR}/cmake/Game.cmake")
