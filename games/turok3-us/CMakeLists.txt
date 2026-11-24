set(RECOMP_ROM "${CMAKE_SOURCE_DIR}/local/turok3.us.z64")
set(RECOMP_SAVE "${CMAKE_SOURCE_DIR}/local/saves/turok3-us.mpk")
set(RECOMP_GAME_GLES2_SUPPORTED ON)
set(RECOMP_GAME_SOURCES
    "../turok2-us/acclaim_pager.c"
    "../turok2-us/acclaim_controls.c")
if(RECOMP_RENDERER STREQUAL "rt64")
    list(APPEND RECOMP_GAME_SOURCES
        "../turok2-us/acclaim_graphics.c"
        "../turok2-us/acclaim_lighting.c")
endif()
include("${CMAKE_SOURCE_DIR}/cmake/Game.cmake")
