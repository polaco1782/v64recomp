#pragma once

#include "runtime_debug.h"
#include "hle/rt64_application.h"
#include "imgui.h"
#include "plume_vulkan.h"

struct RecompDebugGeometryStats {
    uint64_t triangles = 0, draw_calls = 0;
};

/* Read only recorded call descriptors, which remain stable during rendering.
 * Rectangle primitives expand to two triangles but are not guest geometry. */
inline RecompDebugGeometryStats recomp_debug_geometry(const RT64::Workload& workload) {
    RecompDebugGeometryStats stats;
    for (uint32_t f = 0; f < workload.fbPairCount; f++) {
        const auto& pair = workload.fbPairs[f];
        for (uint32_t p = 0; p < pair.projectionCount; p++) {
            const auto& projection = pair.projections[p];
            stats.draw_calls += projection.gameCallCount;
            if (projection.type == RT64::Projection::Type::Rectangle)
                continue;
            for (uint32_t c = 0; c < projection.gameCallCount; c++)
                stats.triangles += projection.gameCalls[c].callDesc.triangleCount;
        }
    }
    return stats;
}

struct RecompDebugGraphicsStats {
    RecompDebugGeometryStats geometry;
    uint64_t tasks = 0, submit_ns = 0;
};

/* The present queue's inspectorMutex serializes this private ImGui context
 * with RT64's inspector and SDL event filter. Runtime reads stay on the owner. */
class RecompDebugOverlay {
public:
    explicit RecompDebugOverlay(RT64::Application& app);
    ~RecompDebugOverlay();
    void update(Runtime *runtime, const RecompDebugGraphicsStats& graphics);
    void draw(plume::RenderCommandList *list, uint64_t frames, uint64_t now);

private:
    RT64::Application& app;
    ImGuiContext *context = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
    RecompDebugStats stats{};
    RecompDebugGraphicsStats graphics{}, sample_graphics{};
    RecompDebugAudioStats sample_audio{};
    uint64_t sample_ns = 0, sample_frames = 0, sample_ops = 0;
    double fps = 0, ops_per_second = 0;
    double dl_per_second = 0, al_per_second = 0, graphics_ms = 0, audio_ms = 0;
};
