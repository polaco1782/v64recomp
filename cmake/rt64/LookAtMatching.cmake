# Respect per-vertex texture generation state before matching look-at entries.
# Keep the pinned checkout untouched and fail if the upstream block changes.
set(rt64_frame_input "${RECOMP_RT64_SOURCE}/src/hle/rt64_game_frame.cpp")
file(READ "${rt64_frame_input}" rt64_frame_source)
set(rt64_lookat_old [=[                    // FIXME: We assume the same look at is used throughout the entire draw call, so we only check the first vertex.
                    const uint32_t curVertexIndex = curWorkload.drawData.faceIndices[curCall.meshDesc.faceIndicesStart];
                    const uint32_t prevVertexIndex = prevWorkload.drawData.faceIndices[prevCall.meshDesc.faceIndicesStart];
                    const uint32_t curLookAtIndex = curWorkload.drawData.lookAtIndices[curVertexIndex] >> RSP_LOOKAT_INDEX_SHIFT;
                    const uint32_t prevLookAtIndex = prevWorkload.drawData.lookAtIndices[prevVertexIndex] >> RSP_LOOKAT_INDEX_SHIFT;
                    lookAtCheckSet.emplace(curLookAtIndex, prevLookAtIndex);]=])
set(rt64_lookat_new [=[                    // Matching below uses the first workload's arrays. Indices from
                    // another workload are not interchangeable, even if in range.
                    if ((curProjIndices.workloadIndex != firstCurProjIndices.workloadIndex) ||
                        (prevProjIndices.workloadIndex != firstPrevProjIndices.workloadIndex)) {
                        continue;
                    }

                    // FIXME: We assume the same look at is used throughout the entire draw call, so we only check the first vertex.
                    if ((curCall.meshDesc.faceIndicesStart >= curWorkload.drawData.faceIndices.size()) ||
                        (prevCall.meshDesc.faceIndicesStart >= prevWorkload.drawData.faceIndices.size())) {
                        continue;
                    }
                    const uint32_t curVertexIndex = curWorkload.drawData.faceIndices[curCall.meshDesc.faceIndicesStart];
                    const uint32_t prevVertexIndex = prevWorkload.drawData.faceIndices[prevCall.meshDesc.faceIndicesStart];
                    if ((curVertexIndex >= curWorkload.drawData.lookAtIndices.size()) ||
                        (prevVertexIndex >= prevWorkload.drawData.lookAtIndices.size())) {
                        continue;
                    }
                    const uint32_t curLookAt = curWorkload.drawData.lookAtIndices[curVertexIndex];
                    const uint32_t prevLookAt = prevWorkload.drawData.lookAtIndices[prevVertexIndex];
                    // Geometry mode at draw time can differ from vertex load time.
                    // Explicit ST modification also disables texture gen per vertex.
                    if (!(curLookAt & RSP_LOOKAT_INDEX_ENABLED) || !(prevLookAt & RSP_LOOKAT_INDEX_ENABLED)) {
                        continue;
                    }
                    const uint32_t curLookAtIndex = curLookAt >> RSP_LOOKAT_INDEX_SHIFT;
                    const uint32_t prevLookAtIndex = prevLookAt >> RSP_LOOKAT_INDEX_SHIFT;
                    if ((curLookAtIndex >= firstCurWorkloadMap.lookAt.size()) ||
                        (prevLookAtIndex >= firstCurWorkloadMap.prevLookAtMapped.size()) ||
                        (curLookAtIndex >= curWorkload.drawData.rspLookAt.size()) ||
                        (prevLookAtIndex >= prevWorkload.drawData.rspLookAt.size()) ||
                        ((firstPrevWorkloadMap != nullptr) && (prevLookAtIndex >= firstPrevWorkloadMap->lookAt.size()))) {
                        continue;
                    }
                    lookAtCheckSet.emplace(curLookAtIndex, prevLookAtIndex);]=])
string(FIND "${rt64_frame_source}" "${rt64_lookat_old}" rt64_lookat_location)
if(rt64_lookat_location EQUAL -1)
    message(FATAL_ERROR "RT64 look-at matching changed; review LookAtMatching.cmake")
endif()
string(REPLACE "${rt64_lookat_old}" "${rt64_lookat_new}" rt64_frame_patched "${rt64_frame_source}")
set(rt64_frame_output "${CMAKE_CURRENT_BINARY_DIR}/lookat-matching/rt64_game_frame.cpp")
file(CONFIGURE OUTPUT "${rt64_frame_output}" CONTENT "${rt64_frame_patched}" @ONLY)
get_target_property(rt64_sources rt64 SOURCES)
list(FIND rt64_sources "${rt64_frame_input}" rt64_frame_index)
if(rt64_frame_index EQUAL -1)
    message(FATAL_ERROR "RT64 game-frame translation unit not found in target sources")
endif()
list(REMOVE_AT rt64_sources ${rt64_frame_index})
list(APPEND rt64_sources "${rt64_frame_output}")
set_property(TARGET rt64 PROPERTY SOURCES "${rt64_sources}")
target_include_directories(rt64 PRIVATE "${RECOMP_RT64_SOURCE}/src/hle")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${rt64_frame_input}")
