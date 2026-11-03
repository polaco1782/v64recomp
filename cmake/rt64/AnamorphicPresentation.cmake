# Native anamorphic cameras project all geometry into the original framebuffer.
# Stretch scanout only; RT64's projection expansion would separate CPU-projected
# backgrounds from RSP models. Keep the pinned checkout and public ABI intact.
set(rt64_vi_input "${RECOMP_RT64_SOURCE}/src/render/rt64_vi_renderer.cpp")
file(READ "${rt64_vi_input}" rt64_vi_source)
set(rt64_vi_header [=[#include "rt64_vi_renderer.h"]=])
set(rt64_vi_header_new [=[#include "rt64_vi_renderer.h"
#include <atomic>

static std::atomic<bool> recomp_anamorphic_present{false};
extern "C" void recomp_rt64_set_anamorphic_present(bool enabled) {
    recomp_anamorphic_present.store(enabled, std::memory_order_relaxed);
}]=])
set(rt64_vi_old [=[        const hlslpp::float2 hdCenter = hdSize / 2;]=])
set(rt64_vi_new [=[        if (recomp_anamorphic_present.load(std::memory_order_relaxed))
            return coordinate * (windowSize / hdSize);
        const hlslpp::float2 hdCenter = hdSize / 2;]=])
foreach(fragment IN ITEMS rt64_vi_header rt64_vi_old)
    string(FIND "${rt64_vi_source}" "${${fragment}}" rt64_vi_location)
    if(rt64_vi_location EQUAL -1)
        message(FATAL_ERROR "RT64 VI presentation changed; review AnamorphicPresentation.cmake")
    endif()
endforeach()
string(REPLACE "${rt64_vi_header}" "${rt64_vi_header_new}" rt64_vi_patched "${rt64_vi_source}")
string(REPLACE "${rt64_vi_old}" "${rt64_vi_new}" rt64_vi_patched "${rt64_vi_patched}")
set(rt64_vi_output "${CMAKE_CURRENT_BINARY_DIR}/anamorphic/rt64_vi_renderer.cpp")
file(CONFIGURE OUTPUT "${rt64_vi_output}" CONTENT "${rt64_vi_patched}" @ONLY)
get_target_property(rt64_sources rt64 SOURCES)
list(FIND rt64_sources "${rt64_vi_input}" rt64_vi_index)
if(rt64_vi_index EQUAL -1)
    message(FATAL_ERROR "RT64 VI translation unit not found in target sources")
endif()
list(REMOVE_AT rt64_sources ${rt64_vi_index})
list(APPEND rt64_sources "${rt64_vi_output}")
set_property(TARGET rt64 PROPERTY SOURCES "${rt64_sources}")
target_include_directories(rt64 PRIVATE "${RECOMP_RT64_SOURCE}/src/render")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${rt64_vi_input}")
