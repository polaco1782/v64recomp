cmake_minimum_required(VERSION 3.20)
project(v64recomp_rt64 LANGUAGES C CXX)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(RT64_STATIC ON CACHE BOOL "" FORCE)
set(RT64_SDL_WINDOW_VULKAN OFF CACHE BOOL "" FORCE)

if(NOT EXISTS "${RECOMP_RT64_SOURCE}/CMakeLists.txt")
    message(FATAL_ERROR "Populate RECOMP_RT64_SOURCE before building RT64")
endif()
add_subdirectory("${RECOMP_RT64_SOURCE}" rt64 EXCLUDE_FROM_ALL)
include("${CMAKE_CURRENT_LIST_DIR}/StableProjection.cmake")

# Keep optimized native renderer stacks usable by the built-in CPU sampler.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(rt64 PRIVATE -g1 -fno-omit-frame-pointer)
    target_compile_options(plume PRIVATE -g1 -fno-omit-frame-pointer)
endif()

# Export the entire static link graph, including platform libraries and include
# requirements. Game builds import these targets rather than configuring RT64.
export(TARGETS texture_hashing rt64 re-spirv nfd libzstd_static plume
    NAMESPACE RecompRT64:: FILE "${CMAKE_BINARY_DIR}/rt64-targets.cmake")
