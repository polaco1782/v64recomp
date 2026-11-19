# Add narrow hooks to the pinned RT64 sources in the shared build directory.
function(recomp_texture_editor_unit path)
    set(input "${RECOMP_RT64_SOURCE}/src/${path}")
    file(READ "${input}" source)
    foreach(fragment IN LISTS ARGN)
        string(FIND "${source}" "${${fragment}}" location)
        if(location EQUAL -1)
            message(FATAL_ERROR "RT64 ${path} changed at ${fragment}; review TextureEditor.cmake")
        endif()
        string(REPLACE "${${fragment}}" "${${fragment}_new}" source "${source}")
    endforeach()
    set(output "${CMAKE_CURRENT_BINARY_DIR}/texture-editor/${path}")
    file(CONFIGURE OUTPUT "${output}" CONTENT "${source}" @ONLY)
    get_target_property(sources rt64 SOURCES)
    if(NOT input IN_LIST sources)
        message(FATAL_ERROR "RT64 ${path} translation unit not found")
    endif()
    list(REMOVE_ITEM sources "${input}")
    list(APPEND sources "${output}")
    set_property(TARGET rt64 PROPERTY SOURCES "${sources}")
    get_filename_component(directory "${input}" DIRECTORY)
    target_include_directories(rt64 PRIVATE "${directory}" "${CMAKE_CURRENT_LIST_DIR}/../../backends")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${input}")
endfunction()

set(geometry_header [=[#include "rt64_framebuffer_renderer.h"]=])
set(geometry_header_new [=[#include "rt64_framebuffer_renderer.h"
#include "texture_editor_hooks.h"

static std::atomic<RecompTextureHooks::Geometry *> textureEditorGeometry{nullptr};
void RecompTextureHooks::setGeometry(Geometry *hook) {
    textureEditorGeometry.store(hook);
}]=])
set(geometry_call [=[                // Determine to use the draw call either in the RT scene or the raster scene.]=])
set(geometry_call_new [=[                if (auto *hook = textureEditorGeometry.load(std::memory_order_relaxed)) {
                    hook(p, call, instanceDrawCall, framebuffer.viewport);
                }

                // Determine to use the draw call either in the RT scene or the raster scene.]=])
recomp_texture_editor_unit(render/rt64_framebuffer_renderer.cpp geometry_header geometry_call)

set(present_header [=[#include "rt64_present_queue.h"]=])
set(present_header_new [=[#include "rt64_present_queue.h"
#include "texture_editor_hooks.h"

static std::atomic<RecompTextureHooks::Present *> textureEditorPresent{nullptr};
static std::atomic<bool (*)()> textureEditorFrozen{nullptr};
void RecompTextureHooks::setPresent(Present *hook, bool (*frozen)()) {
    textureEditorFrozen.store(frozen);
    textureEditorPresent.store(hook);
}]=])
set(present_operations [=[        // Perform any external write operations indicated by the event.
        if (!present.fbOperations.empty()) {]=])
set(present_operations_new [=[        auto *frozen = textureEditorFrozen.load(std::memory_order_relaxed);
        const bool editorPaused = present.paused && frozen && frozen();
        uint32_t editorFbAddress = UINT32_MAX;
        // Perform any external write operations indicated by the event.
        if (!editorPaused && !present.fbOperations.empty()) {]=])
set(present_visible [=[        if (present.screenVI.visible()) {]=])
set(present_visible_new [=[        if (!editorPaused && present.screenVI.visible()) {]=])
set(present_address [=[                RenderTargetKey colorTargetKey(presentFb->addressStart,]=])
set(present_address_new [=[                editorFbAddress = presentFb->addressStart;
                RenderTargetKey colorTargetKey(presentFb->addressStart,]=])
set(present_render [=[                commandList->setFramebuffer(swapChainFramebuffer);
                commandList->clearColor();]=])
set(present_render_new [=[                if (auto *hook = textureEditorPresent.load(std::memory_order_relaxed)) {
                    hook(renderParams, present, editorFbAddress);
                }

                commandList->setFramebuffer(swapChainFramebuffer);
                commandList->clearColor();]=])
# Value-initialize the entire descriptor for blank VIs and provide its list to
# the paused hook, which supplies the frozen VI source.
set(present_params [=[                VIRenderer::RenderParams renderParams;]=])
set(present_params_new [=[                VIRenderer::RenderParams renderParams{};
                renderParams.commandList = commandList;]=])
set(present_counter [=[            else if (framesToPresent == 1) {]=])
set(present_counter_new [=[            else if (!editorPaused && (framesToPresent == 1)) {]=])
recomp_texture_editor_unit(hle/rt64_present_queue.cpp present_header present_operations present_visible
    present_address present_render present_params present_counter)

set(scanner_header [=[#include "rt64_texture_pack.h"]=])
set(scanner_header_new [=[#include "rt64_texture_pack.h"
#include "texture_editor_hooks.h"
#include "texture_editor_paths.h"

static std::atomic<RecompTextureHooks::Rice *> textureEditorRice{nullptr};
void RecompTextureHooks::setRice(Rice *hook) {
    textureEditorRice.store(hook);
}]=])
set(scanner_cache [=[        // A stale or invalid cache is disposable. Never trust mappings from another pack/title/version.]=])
set(scanner_cache_new [=[        // Overrides using the current RT64 hash are usable even when a draw
        // has no recoverable Rice identity (for example, direct TMEM uploads).
        for (const auto& name : *source) {
            if (!RecompTexturePaths::isRT64Override(name))
                continue;
            ReplacementTexture texture;
            texture.hashes.rt64 = name.substr(name.size() - 20, 16);
            texture.path = name;
            texture.operation = ReplacementOperation::Stream;
            paths.emplace(name, Image{fs::u8path(name), {}, false, false});
            db.addReplacement(texture);
            seen.insert(ReplacementDatabase::stringToHash(texture.hashes.rt64));
        }
        // A stale or invalid cache is disposable. Never trust mappings from another pack/title/version.]=])
set(scanner_cached_entry [=[                            texture.operation = ReplacementOperation::Stream;
                            db.addReplacement(texture);
                            seen.insert(ReplacementDatabase::stringToHash(texture.hashes.rt64));]=])
set(scanner_cached_entry_new [=[                            const uint64_t hash = ReplacementDatabase::stringToHash(texture.hashes.rt64);
                            if (seen.count(hash))
                                continue;
                            texture.operation = ReplacementOperation::Stream;
                            db.addReplacement(texture);
                            seen.insert(hash);
                            if (auto *hook = textureEditorRice.load(std::memory_order_relaxed))
                                hook(hash, texture.hashes.rice);]=])
set(scanner_rice [=[        const auto key = matchKey(images, rice);]=])
set(scanner_rice_new [=[        if (auto *hook = textureEditorRice.load(std::memory_order_relaxed))
            hook(capture.hash, rice);
        const auto key = matchKey(images, rice);]=])
set(scanner_override [=[    bool ordinary = std::any_of(images.begin(), images.end(), [](const Image& image) { return !image.alternate; });]=])
set(scanner_override_new [=[    const Image *override = nullptr;
    for (const auto& image : images) {
        if (image.path.parent_path().generic_u8string() == RecompTexturePaths::RiceOverrides &&
            (!override || image.path < override->path))
            override = &image;
    }
    if (override)
        return *override;
    bool ordinary = std::any_of(images.begin(), images.end(), [](const Image& image) { return !image.alternate; });]=])
set(scanner_empty [=[        if (images.empty())
            throw std::runtime_error("ZIP contains no supported Rice PNG filenames for ROM title: " + title);]=])
# A ZIP of plain PNGs can be used as an editor library before any hash is mapped.
set(scanner_empty_new "")
recomp_texture_editor_unit(common/rt64_texture_pack.cpp scanner_header scanner_cache scanner_cached_entry
    scanner_rice scanner_override scanner_empty)
