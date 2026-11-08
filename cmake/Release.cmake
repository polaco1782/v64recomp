# Keep translated CPU code compact without changing scheduling or FP semantics.
# Handwritten runtime/backends retain their normal Release optimization level.
option(RECOMP_OPTIMIZE_SIZE "Reduce generated Release CPU code size and discard unused sections" ON)
option(RECOMP_SEPARATE_DEBUG_INFO "Store Linux Release debug information in a matching .debug file" ON)

function(recomp_optimize_release_target target)
    if(NOT RECOMP_OPTIMIZE_SIZE OR NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        return()
    endif()
    if(MSVC)
        target_compile_options(${target} PRIVATE /Gy /Gw)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_options(${target} PRIVATE -ffunction-sections -fdata-sections)
        get_target_property(type ${target} TYPE)
        if(type STREQUAL "EXECUTABLE")
            target_link_options(${target} PRIVATE LINKER:--gc-sections)
        endif()
    endif()
endfunction()

function(recomp_optimize_release_cpu)
    if(NOT RECOMP_OPTIMIZE_SIZE OR NOT CMAKE_BUILD_TYPE STREQUAL "Release")
        return()
    endif()
    if(MSVC)
        set(flags /O1 /Os /GL-)
    elseif(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
        include(CheckCCompilerFlag)
        check_c_compiler_flag(-Oz RECOMP_COMPILER_HAS_OZ)
        if(RECOMP_COMPILER_HAS_OZ)
            set(flags -Oz -fno-lto)
        else()
            set(flags -Os -fno-lto)
        endif()
    else()
        return()
    endif()
    # Source options follow target/global options, including an inherited IPO
    # setting. CPU helper inlining across modules substantially expands code.
    set_property(SOURCE ${ARGN} APPEND PROPERTY COMPILE_OPTIONS ${flags})
endfunction()

function(recomp_separate_release_debug target)
    if(NOT RECOMP_SEPARATE_DEBUG_INFO OR NOT CMAKE_BUILD_TYPE STREQUAL "Release"
            OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
        return()
    endif()
    if(NOT CMAKE_OBJCOPY OR NOT CMAKE_STRIP)
        message(FATAL_ERROR "Separate Release debug information requires CMAKE_OBJCOPY and CMAKE_STRIP")
    endif()
    set(debug_dir "${CMAKE_BINARY_DIR}/bin")
    if(CMAKE_CONFIGURATION_TYPES)
        string(APPEND debug_dir "/$<CONFIG>")
    endif()
    set(debug_file "${debug_dir}/${target}${CMAKE_EXECUTABLE_SUFFIX}.debug")
    set(script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/SeparateDebug.cmake")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${script}")
    # Register before texture embedding: objcopy/strip invalidate appended ZIP
    # offsets. A missing sidecar must also cause a fresh link, not strip twice.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_COMMAND}" "-DINPUT=$<TARGET_FILE:${target}>"
            "-DDEBUG_OUTPUT=${debug_file}" "-DOBJCOPY=${CMAKE_OBJCOPY}" "-DSTRIP=${CMAKE_STRIP}"
            -P "${script}"
        BYPRODUCTS "${debug_file}"
        COMMENT "Separating ${target} Release debug information" VERBATIM)
endfunction()
