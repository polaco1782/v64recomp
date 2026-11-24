set(RECOMP_ROM "${CMAKE_SOURCE_DIR}/local/turok2.us.z64")
set(RECOMP_SAVE "${CMAKE_SOURCE_DIR}/local/saves/turok2-us.mpk")
set(RECOMP_GAME_GLES2_SUPPORTED ON)
set(RECOMP_GAME_SOURCES
    "acclaim_pager.c"
    "acclaim_controls.c")
if(RECOMP_RENDERER STREQUAL "rt64")
    list(APPEND RECOMP_GAME_SOURCES
        "acclaim_graphics.c"
        "acclaim_lighting.c")
endif()
include("${CMAKE_SOURCE_DIR}/cmake/Game.cmake")
