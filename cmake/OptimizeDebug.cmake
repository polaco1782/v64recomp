# Included by Game.cmake in the game's subdirectory, after target creation.
# Keep RECOMP_DEBUG, assertions, symbols and the Debug CRT/ABI. Only optimization
# and incompatible compiler runtime-check flags change; RT64 stays in its shared
# cache with its existing build configuration.
message(STATUS "Optimized Debug enabled for ${RECOMP_GAME}; use RECOMP_OPTIMIZE_DEBUG=OFF for source stepping")
if(MSVC)
    foreach(language C CXX)
        set(debug_flags "${CMAKE_${language}_FLAGS_DEBUG}")
        # CMake 3.x / the legacy MSVC policy puts these switches in the flags
        # cache. Remove them from this configuration's local scopes, without
        # rewriting the cache, so disabling the option restores the defaults.
        while(debug_flags MATCHES "(^|[ \t])[-/](Od|Ob0|RTC[1csu]+)([ \t]|$)")
            string(REGEX REPLACE "(^|[ \t])[-/](Od|Ob0|RTC[1csu]+)([ \t]|$)" " " debug_flags "${debug_flags}")
        endwhile()
        string(STRIP "${debug_flags}" debug_flags)
        set(CMAKE_${language}_FLAGS_DEBUG "${debug_flags}")
        # Shared runtime/backend targets belong to the root directory.
        set(CMAKE_${language}_FLAGS_DEBUG "${debug_flags}" PARENT_SCOPE)
    endforeach()
endif()
foreach(target ${RECOMP_GAME} recomp_runtime recomp_rsp_memory recomp_jpeg recomp_audio recomp_renderer)
    if(TARGET "${target}")
        if(MSVC)
            target_compile_options(${target} PRIVATE /O2 /Ob2)
            # Also cover CMake 4's runtime-check policy when enabled externally.
            set_property(TARGET ${target} PROPERTY MSVC_RUNTIME_CHECKS "")
        else()
            target_compile_options(${target} PRIVATE -O2)
        endif()
    endif()
endforeach()
