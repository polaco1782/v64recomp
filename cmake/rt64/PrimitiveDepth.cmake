# Primitive Z is unsigned 0.15 fixed point. Its maximum must remain below the
# cleared D32 depth (1.0), including for depth-tested far-plane rectangles.
# Keep the pinned checkout untouched and guard the replacement against drift.
set(rt64_rdp_input "${RECOMP_RT64_SOURCE}/src/hle/rt64_rdp.cpp")
file(READ "${rt64_rdp_input}" rt64_rdp_source)
set(rt64_primitive_depth_old "        const float Fixed15ToFloat = 1.0f / 32767.0f;")
set(rt64_primitive_depth_new [=[        // Primitive Z is 0.15 fixed point, not an inclusive [0, 1] UNORM.
        // Mapping 0x7fff to 1.0 rejects far-depth draws against a cleared
        // depth buffer under LESS (Hexen uses this to mask its sky).
        const float Fixed15ToFloat = 1.0f / 32768.0f;]=])
string(FIND "${rt64_rdp_source}" "${rt64_primitive_depth_old}" rt64_primitive_depth_location)
if(rt64_primitive_depth_location EQUAL -1)
    message(FATAL_ERROR "RT64 primitive-depth calculation changed; review PrimitiveDepth.cmake")
endif()
string(REPLACE "${rt64_primitive_depth_old}" "${rt64_primitive_depth_new}" rt64_rdp_patched "${rt64_rdp_source}")
set(rt64_rdp_output "${CMAKE_CURRENT_BINARY_DIR}/primitive-depth/rt64_rdp.cpp")
file(CONFIGURE OUTPUT "${rt64_rdp_output}" CONTENT "${rt64_rdp_patched}" @ONLY)
get_target_property(rt64_sources rt64 SOURCES)
list(FIND rt64_sources "${rt64_rdp_input}" rt64_rdp_index)
if(rt64_rdp_index EQUAL -1)
    message(FATAL_ERROR "RT64 RDP translation unit not found in target sources")
endif()
list(REMOVE_AT rt64_sources ${rt64_rdp_index})
list(APPEND rt64_sources "${rt64_rdp_output}")
set_property(TARGET rt64 PROPERTY SOURCES "${rt64_sources}")
target_include_directories(rt64 PRIVATE "${RECOMP_RT64_SOURCE}/src/hle")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${rt64_rdp_input}")
