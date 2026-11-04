set(RECOMP_ROM "${CMAKE_SOURCE_DIR}/local/goldeneye.us.z64")
set(RECOMP_SAVE "${CMAKE_SOURCE_DIR}/local/saves/goldeneye-us.eeprom")
set(RECOMP_GAME_SOURCES pager.c graphics.cpp input.c audio_dma.c fov.c)
include("${CMAKE_SOURCE_DIR}/cmake/Game.cmake")

# Let Release specialize the constant register/operation arguments passed from
# translated code into the checked runtime helpers. Keep strict FP semantics
# and the shared RT64 build unchanged; unsupported toolchains keep normal codegen.
include(CheckIPOSupported)
check_ipo_supported(RESULT goldeneye_ipo_supported OUTPUT goldeneye_ipo_error LANGUAGES C CXX)
if(goldeneye_ipo_supported)
    set_property(TARGET goldeneye-us recomp_runtime PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
    target_compile_definitions(recomp_runtime PRIVATE "$<$<CONFIG:Release>:RECOMP_FPU_SPECIALIZE>")
else()
    message(STATUS "GoldenEye Release IPO unavailable: ${goldeneye_ipo_error}")
endif()
