option(RECOMP_BUILD_TEXTURE_SCAN_TESTS "Build native texture scanner tests" OFF)
if(RECOMP_BUILD_TEXTURE_SCAN_TESTS)
    add_executable(texture_scan_tests tests.cpp)
    target_link_libraries(texture_scan_tests PRIVATE recomp_renderer)
    target_include_directories(texture_scan_tests SYSTEM PRIVATE ${RECOMP_RT64_INCLUDES})
    target_compile_options(texture_scan_tests PRIVATE -Wall -Wextra -Werror)
    add_custom_target(check-texture-scan
        COMMAND $<TARGET_FILE:texture_scan_tests> $<TARGET_FILE:texture_hasher>
        DEPENDS texture_scan_tests USES_TERMINAL)
endif()
