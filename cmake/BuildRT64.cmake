# Serialize configuration and compilation across independent game build trees.
file(MAKE_DIRECTORY "${RECOMP_RT64_BUILD_DIR}")
file(LOCK "${RECOMP_RT64_BUILD_DIR}.lock" GUARD PROCESS TIMEOUT 1800)
if(RECOMP_RT64_CONFIGURE_ONLY OR NOT EXISTS "${RECOMP_RT64_BUILD_DIR}/CMakeCache.txt")
    set(generator_options)
    if(RECOMP_RT64_PLATFORM)
        list(APPEND generator_options -A "${RECOMP_RT64_PLATFORM}")
    endif()
    if(RECOMP_RT64_TOOLSET)
        list(APPEND generator_options -T "${RECOMP_RT64_TOOLSET}")
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}"
        -S "${CMAKE_CURRENT_LIST_DIR}/rt64" -B "${RECOMP_RT64_BUILD_DIR}"
        -G "${RECOMP_RT64_GENERATOR}" -C "${RECOMP_RT64_INITIAL_CACHE}"
        ${generator_options}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Shared RT64 configuration failed:\n${output}\n${errors}")
    endif()
endif()
if(NOT RECOMP_RT64_CONFIGURE_ONLY)
    execute_process(COMMAND "${CMAKE_COMMAND}" --build "${RECOMP_RT64_BUILD_DIR}"
        --config "${RECOMP_RT64_BUILD_TYPE}"
        --target rt64 --parallel "${RECOMP_RT64_JOBS}"
        RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Shared RT64 build failed (${result})")
    endif()
endif()
