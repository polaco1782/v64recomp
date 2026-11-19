#pragma once
#include "texture_editor_hooks.h"
#include "texture_editor_zip.h"
#include "imgui.h"
#include "plume_vulkan.h"
#include <array>
#include <mutex>
#include <optional>
#include <unordered_map>

class RecompTextureFiles;

namespace RT64 {
struct Application;
struct VI;
}

class RecompTextureEditor {
public:
    RecompTextureEditor(RT64::Application&, RecompTextureFiles&);
    ~RecompTextureEditor();
    void run();
    bool quitRequested() const {
        return quit;
    }
    bool open() const {
        return editing.load();
    }
    void draw(plume::RenderCommandList *);
    void recordGeometry(const RT64::FramebufferRenderer::DrawParams&, const RT64::GameCall&,
                        const RT64::InstanceDrawCall&, const plume::RenderViewport&);
    void recordPresent(RT64::VIRenderer::RenderParams&, const RT64::Present&, uint32_t);
    void recordRice(uint64_t, const std::string&);

private:
    struct Shape {
        struct Vertex {
            float x, y, depth;
        };
        std::array<Vertex, 4> points;
        std::array<uint64_t, 8> hashes{};
        unsigned hashCount = 0;
        plume::RenderRect scissor;
        uint32_t callIndex;
        unsigned count;
        bool rectangle = false;
        bool depthCompare, depthWrite, clampNear;
    };

    struct GeometryFrame {
        uint64_t workload = 0;
        std::unordered_map<uint32_t, std::vector<Shape>> framebuffers;
    };

    struct Preview {
        std::unique_ptr<plume::RenderTexture> texture;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
        int width = 0, height = 0;
        std::string label;
    };

    void window();
    void pick();
    void uploadPreview(Preview&, const RecompTextureZip::Image&, const std::string& label);
    void clearPreview(Preview&);
    void showPreview(const Preview&);
    void selectTexture(uint64_t);
    void selectPackImage(const std::string&);
    RecompTextureZip::Image packImage(const std::string&);
    void listEntries();
    void save(const std::string& entry, const std::vector<uint8_t>& png);
    void importTexture();
    void replaceTexture();
    void frame();

    RT64::Application& app;
    RecompTextureFiles& files;
    ImGuiContext *context = nullptr;
    VkDevice device = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    std::atomic<bool> editing{false};
    bool quit = false;
    bool showBrowser = false;
    bool requestImport = false, requestReplace = false;
    std::optional<uint64_t> requestTexture;
    std::string requestPackImage;
    bool hasPresentation = false;
    std::mutex geometryMutex, riceMutex;
    std::array<GeometryFrame, 4> geometry;
    std::vector<Shape> frozenGeometry;
    std::unordered_map<uint64_t, std::string> riceHashes;
    std::unique_ptr<plume::RenderTexture> frozen;
    RT64::VIRenderer::RenderParams frozenParams;
    std::unique_ptr<RT64::VI> frozenVI;
    Preview selectedPreview, packPreview;
    ImGuiTextFilter filter;
    std::vector<std::string> entries;
    std::vector<uint64_t> selection;
    uint64_t selectedHash = 0;
    std::string packSelection, status;
};
