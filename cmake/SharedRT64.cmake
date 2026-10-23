# One independent RT64 build per compatible compiler/configuration, shared by
# every game. The game runtime and renderer adapter still use their own mode.
set(RECOMP_RT64_BUILD_ROOT "${CMAKE_SOURCE_DIR}/build/shared/rt64" CACHE PATH "Shared RT64 build cache")
get_filename_component(RECOMP_RT64_SOURCE "${RECOMP_RT64_SOURCE}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
get_filename_component(RECOMP_RT64_BUILD_ROOT "${RECOMP_RT64_BUILD_ROOT}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
set(RECOMP_RT64_BUILD_TYPE RelWithDebInfo CACHE STRING "Shared RT64 configuration")
set_property(CACHE RECOMP_RT64_BUILD_TYPE PROPERTY STRINGS Release RelWithDebInfo Debug)
if(NOT RECOMP_RT64_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|Debug)$")
    message(FATAL_ERROR "RECOMP_RT64_BUILD_TYPE must be Release, RelWithDebInfo or Debug")
endif()
set(RECOMP_RT64_JOBS 4 CACHE STRING "Parallel compiler jobs in the shared RT64 build")
if(NOT RECOMP_RT64_JOBS MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "RECOMP_RT64_JOBS must be a positive integer")
endif()
string(TOUPPER "${RECOMP_RT64_BUILD_TYPE}" rt64_config)
set(rt64_settings RECOMP_RT64_SOURCE CMAKE_C_COMPILER CMAKE_CXX_COMPILER
    CMAKE_C_COMPILER_ARG1 CMAKE_CXX_COMPILER_ARG1
    CMAKE_C_COMPILER_LAUNCHER CMAKE_CXX_COMPILER_LAUNCHER
    CMAKE_C_COMPILER_TARGET CMAKE_CXX_COMPILER_TARGET
    CMAKE_C_COMPILER_EXTERNAL_TOOLCHAIN CMAKE_CXX_COMPILER_EXTERNAL_TOOLCHAIN
    CMAKE_C_FLAGS CMAKE_CXX_FLAGS CMAKE_C_FLAGS_${rt64_config} CMAKE_CXX_FLAGS_${rt64_config}
    CMAKE_EXE_LINKER_FLAGS CMAKE_SHARED_LINKER_FLAGS CMAKE_STATIC_LINKER_FLAGS
    CMAKE_INTERPROCEDURAL_OPTIMIZATION CMAKE_INTERPROCEDURAL_OPTIMIZATION_${rt64_config}
    CMAKE_TOOLCHAIN_FILE CMAKE_SYSROOT CMAKE_SYSROOT_COMPILE CMAKE_SYSROOT_LINK
    CMAKE_FIND_ROOT_PATH CMAKE_FIND_ROOT_PATH_MODE_PROGRAM CMAKE_FIND_ROOT_PATH_MODE_LIBRARY
    CMAKE_FIND_ROOT_PATH_MODE_INCLUDE CMAKE_FIND_ROOT_PATH_MODE_PACKAGE
    CMAKE_PREFIX_PATH CMAKE_LIBRARY_PATH CMAKE_INCLUDE_PATH
    CMAKE_OSX_ARCHITECTURES CMAKE_OSX_SYSROOT CMAKE_OSX_DEPLOYMENT_TARGET
    CMAKE_MSVC_RUNTIME_LIBRARY)
if(CMAKE_CROSSCOMPILING)
    list(APPEND rt64_settings CMAKE_SYSTEM_NAME CMAKE_SYSTEM_PROCESSOR CMAKE_SYSTEM_VERSION)
endif()
set(rt64_cache "set(CMAKE_BUILD_TYPE [==[${RECOMP_RT64_BUILD_TYPE}]==] CACHE STRING \"\" FORCE)\n")
set(rt64_signature "${CMAKE_GENERATOR}|${CMAKE_SYSTEM_NAME}|${CMAKE_SYSTEM_PROCESSOR}|${CMAKE_SIZEOF_VOID_P}|${CMAKE_C_COMPILER_VERSION}|${CMAKE_CXX_COMPILER_VERSION}\n${rt64_cache}")
foreach(setting IN LISTS rt64_settings)
    if(DEFINED ${setting})
        string(APPEND rt64_cache "set(${setting} [==[${${setting}}]==] CACHE STRING \"\" FORCE)\n")
        string(APPEND rt64_signature "${setting}=${${setting}}\n")
    endif()
endforeach()
if(CMAKE_TOOLCHAIN_FILE AND EXISTS "${CMAKE_TOOLCHAIN_FILE}")
    file(SHA256 "${CMAKE_TOOLCHAIN_FILE}" rt64_toolchain_hash)
    string(APPEND rt64_signature "toolchain=${rt64_toolchain_hash}\n")
endif()
string(SHA256 rt64_key "${rt64_signature}")
string(SUBSTRING "${rt64_key}" 0 16 rt64_key)
set(RECOMP_RT64_BUILD_DIR "${RECOMP_RT64_BUILD_ROOT}/${rt64_key}")
set(rt64_cache_file "${CMAKE_BINARY_DIR}/rt64-initial-cache.cmake")
file(CONFIGURE OUTPUT "${rt64_cache_file}" CONTENT "${rt64_cache}" @ONLY)
set(rt64_command "${CMAKE_COMMAND}"
    "-DRECOMP_RT64_BUILD_DIR=${RECOMP_RT64_BUILD_DIR}"
    "-DRECOMP_RT64_INITIAL_CACHE=${rt64_cache_file}"
    "-DRECOMP_RT64_GENERATOR=${CMAKE_GENERATOR}"
    "-DRECOMP_RT64_JOBS=${RECOMP_RT64_JOBS}")
message(STATUS "Shared RT64 (${RECOMP_RT64_BUILD_TYPE}): ${RECOMP_RT64_BUILD_DIR}")
execute_process(COMMAND ${rt64_command} -DRECOMP_RT64_CONFIGURE_ONLY=ON
    -P "${CMAKE_CURRENT_LIST_DIR}/BuildRT64.cmake" RESULT_VARIABLE rt64_result)
if(NOT rt64_result EQUAL 0)
    message(FATAL_ERROR "Could not configure the shared RT64 build")
endif()
include("${RECOMP_RT64_BUILD_DIR}/rt64-targets.cmake")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/rt64/CMakeLists.txt" "${CMAKE_CURRENT_LIST_DIR}/BuildRT64.cmake"
    "${CMAKE_CURRENT_LIST_DIR}/rt64/StableProjection.cmake"
    "${CMAKE_CURRENT_LIST_DIR}/rt64/PrimitiveDepth.cmake"
    "${CMAKE_CURRENT_LIST_DIR}/rt64/LookAtMatching.cmake")

# Local symlinks are build byproducts so Ninja can order missing libraries.
# Cleaning one game removes only these links, preserving the shared artifacts.
set(rt64_link_dir "${CMAKE_BINARY_DIR}/rt64-links")
set(rt64_link_commands)
set(rt64_links)
foreach(library rt64 re-spirv nfd libzstd_static plume)
    set(target "RecompRT64::${library}")
    get_target_property(archive "${target}" "IMPORTED_LOCATION_${rt64_config}")
    get_filename_component(filename "${archive}" NAME)
    set(link "${rt64_link_dir}/${filename}")
    set_target_properties("${target}" PROPERTIES "IMPORTED_LOCATION_${rt64_config}" "${link}"
        MAP_IMPORTED_CONFIG_DEBUG "${rt64_config}" MAP_IMPORTED_CONFIG_RELEASE "${rt64_config}")
    list(APPEND rt64_links "${link}")
    list(APPEND rt64_link_commands COMMAND "${CMAKE_COMMAND}" -E create_symlink "${archive}" "${link}")
endforeach()
add_custom_target(recomp_rt64
    COMMAND ${rt64_command} -P "${CMAKE_CURRENT_LIST_DIR}/BuildRT64.cmake"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${rt64_link_dir}"
    ${rt64_link_commands}
    BYPRODUCTS ${rt64_links}
    COMMENT "Checking shared RT64 libraries" USES_TERMINAL VERBATIM)
foreach(library rt64 re-spirv nfd libzstd_static plume)
    add_dependencies("RecompRT64::${library}" recomp_rt64)
endforeach()
