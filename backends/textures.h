#pragma once
#include <filesystem>
#include <unordered_set>
#include "render/rt64_texture_cache.h"

class RecompTextureFiles {
    std::filesystem::path dumpDirectory, replacementDirectory;
    std::unordered_set<uint64_t> dumped, replaced;
public:
    uint64_t pngsWritten = 0, replacementFiles = 0;
    void configure(RT64::TextureCache &cache);
    void reload(RT64::TextureCache &cache);
    void refreshAndToggle(RT64::TextureCache &cache);
    void collect(RT64::TextureCache &cache);
    uint64_t replacedCount() const { return replaced.size(); }
};
