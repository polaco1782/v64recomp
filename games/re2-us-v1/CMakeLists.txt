set(RECOMP_ROM "${CMAKE_SOURCE_DIR}/local/re2.us.rev1.z64")
set(RECOMP_SAVE "${CMAKE_SOURCE_DIR}/local/saves/re2-us-v1.mpk")
set(RECOMP_GAME_SOURCES run.c video.c video_dsp.c)
include("${CMAKE_SOURCE_DIR}/cmake/Game.cmake")
