#pragma once
#include <atomic>
#include <cstdint>
#include <string>

namespace RT64 {
struct Present;
}

#include "render/rt64_framebuffer_renderer.h"
#include "render/rt64_vi_renderer.h"

namespace RecompTextureHooks {
using Geometry = void(const RT64::FramebufferRenderer::DrawParams&, const RT64::GameCall&,
                      const RT64::InstanceDrawCall&, const plume::RenderViewport&);
using Present = void(RT64::VIRenderer::RenderParams&, const RT64::Present&, uint32_t);
using Rice = void(uint64_t, const std::string&);

void setGeometry(Geometry *);
void setPresent(Present *, bool (*frozen)());
void setRice(Rice *);
}
