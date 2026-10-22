# Keep the pinned RT64 checkout untouched. Replace only the verified unstable
# depth-coefficient calculation in the build's copy of the math translation unit.
set(rt64_math_input "${RECOMP_RT64_SOURCE}/src/common/rt64_math.cpp")
file(READ "${rt64_math_input}" rt64_math_source)
set(rt64_depth_old "        p[2][2] = vp[0][2] / v[0][2];")
set(rt64_depth_new [=[        // Pivot on the largest view-axis component. A fixed X pivot can be
        // zero at the starting camera, or amplify 16.16 quantization on turns.
        unsigned depthAxis = 0;
        for (unsigned axis = 1; axis < 3; axis++) {
            if (std::abs(float(v[axis][2])) > std::abs(float(v[depthAxis][2])))
                depthAxis = axis;
        }
        if (float(v[depthAxis][2]) == 0.0f) {
            v = hlslpp::float4x4::identity();
            p = vp;
            return;
        }
        p[2][2] = vp[depthAxis][2] / v[depthAxis][2];]=])
string(FIND "${rt64_math_source}" "${rt64_depth_old}" rt64_depth_location)
if(rt64_depth_location EQUAL -1)
    message(FATAL_ERROR "RT64 view-projection calculation changed; review StableProjection.cmake")
endif()
string(REPLACE "${rt64_depth_old}" "${rt64_depth_new}" rt64_math_patched "${rt64_math_source}")
set(rt64_math_output "${CMAKE_CURRENT_BINARY_DIR}/stable-projection/rt64_math.cpp")
file(CONFIGURE OUTPUT "${rt64_math_output}" CONTENT "${rt64_math_patched}" @ONLY)
get_target_property(rt64_sources rt64 SOURCES)
list(FIND rt64_sources "${rt64_math_input}" rt64_math_index)
if(rt64_math_index EQUAL -1)
    message(FATAL_ERROR "RT64 math translation unit not found in target sources")
endif()
list(REMOVE_AT rt64_sources ${rt64_math_index})
list(APPEND rt64_sources "${rt64_math_output}")
set_property(TARGET rt64 PROPERTY SOURCES "${rt64_sources}")
# The original unit includes its header relative to src/common.
target_include_directories(rt64 PRIVATE "${RECOMP_RT64_SOURCE}/src/common")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${rt64_math_input}")
