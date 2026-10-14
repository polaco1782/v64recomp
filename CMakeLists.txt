cmake_minimum_required(VERSION 3.20)
project(v64recomp LANGUAGES C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

find_package(OpenSSL 3 REQUIRED COMPONENTS Crypto)
find_package(Threads REQUIRED)
add_library(recomp_runtime STATIC runtime/memory.c runtime/fpu.c runtime/scheduler.c runtime/worker.c runtime/os.c
    runtime/bootstrap.c runtime/time.c runtime/platform.c runtime/tasks.c runtime/io.c runtime/video.c)
target_link_libraries(recomp_runtime PUBLIC OpenSSL::Crypto Threads::Threads m ${CMAKE_DL_LIBS})
target_include_directories(recomp_runtime PUBLIC include)
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(recomp_runtime PRIVATE -Wall -Wextra -Werror -fno-fast-math -frounding-math -ffp-contract=off)
endif()

option(RECOMP_BUILD_AUDIO "Build native ABI1 synthesis and SDL playback" ON)
if(RECOMP_BUILD_AUDIO)
    find_package(SDL2 REQUIRED)
    add_library(recomp_audio STATIC backends/audio.c third_party/rsp-hle/alist.c
        third_party/rsp-hle/alist_audio.c third_party/rsp-hle/audio.c third_party/rsp-hle/memory.c)
    target_include_directories(recomp_audio PRIVATE third_party/rsp-hle)
    target_link_libraries(recomp_audio PUBLIC recomp_runtime SDL2::SDL2)
    target_compile_options(recomp_audio PRIVATE -Wall -Wextra -Werror -fno-strict-aliasing)
endif()

option(RECOMP_BUILD_RT64 "Build the standalone RT64 graphics adapter" OFF)
if(RECOMP_BUILD_RT64)
    enable_language(CXX)
    set(CMAKE_CXX_STANDARD 17)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    set(RECOMP_RT64_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/rt64" CACHE PATH "Existing RT64 source checkout")
    set(RT64_STATIC ON CACHE BOOL "" FORCE)
    set(RT64_SDL_WINDOW_VULKAN OFF CACHE BOOL "" FORCE)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    add_subdirectory("${RECOMP_RT64_SOURCE}" "${CMAKE_CURRENT_BINARY_DIR}/rt64" EXCLUDE_FROM_ALL)
    find_package(SDL2 REQUIRED)
    add_library(recomp_renderer STATIC backends/rt64.cpp backends/textures.cpp)
    target_link_libraries(recomp_renderer PUBLIC recomp_runtime rt64 SDL2::SDL2)
    target_compile_definitions(recomp_renderer PRIVATE HLSL_CPU FFX_GCC IMGUI_IMPL_VULKAN_NO_PROTOTYPES)
    target_compile_options(recomp_renderer PRIVATE -Wall -Wextra -Werror)
    set(RECOMP_RT64_INCLUDES
        "${RECOMP_RT64_SOURCE}/src" "${RECOMP_RT64_SOURCE}/src/contrib"
        "${RECOMP_RT64_SOURCE}/src/contrib/plume" "${RECOMP_RT64_SOURCE}/src/contrib/imgui"
        "${RECOMP_RT64_SOURCE}/src/contrib/hlslpp/include"
        "${RECOMP_RT64_SOURCE}/src/contrib/mupen64plus-core/src/api")
    target_include_directories(recomp_renderer SYSTEM PRIVATE ${RECOMP_RT64_INCLUDES})
endif()

# Keep generated game code inspectable in Debug builds, including local variables.
set(RECOMP_GAME_OPTIMIZATION
    "$<$<CONFIG:Debug>:-O0;-g3>"
    "$<$<NOT:$<CONFIG:Debug>>:-O2;-g1>")

add_subdirectory(games/sm64-us)
add_subdirectory(games/goldeneye-us)
add_subdirectory(games/shadowman-us)
add_subdirectory(games/quake-us)
add_subdirectory(games/duke-us)

add_subdirectory(games/nightmare-us)
add_subdirectory(games/oot-us-v12)

include(CTest)
if(BUILD_TESTING)
    add_executable(recomp_threading_test tests/threading.c)
    target_link_libraries(recomp_threading_test PRIVATE recomp_runtime)
    target_compile_options(recomp_threading_test PRIVATE -Wall -Wextra -Werror)
    add_test(NAME runtime_threading COMMAND recomp_threading_test)
    set_tests_properties(runtime_threading PROPERTIES TIMEOUT 30)
    if(RECOMP_BUILD_AUDIO)
        add_executable(recomp_audio_worker_test tests/audio_worker.c)
        target_link_libraries(recomp_audio_worker_test PRIVATE recomp_audio)
        target_compile_options(recomp_audio_worker_test PRIVATE -Wall -Wextra -Werror)
        add_test(NAME audio_worker COMMAND recomp_audio_worker_test)
        set_tests_properties(audio_worker PROPERTIES TIMEOUT 30)
    endif()
endif()
