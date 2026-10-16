#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace RecompTextures {
// Reads pack entries from an extracted directory or a ZIP without extraction.
class Source {
    struct Impl;
    std::unique_ptr<Impl> impl;

public:
    explicit Source(const std::filesystem::path& path);
    ~Source();
    std::vector<std::filesystem::path> paths() const;
    std::vector<uint8_t> read(const std::filesystem::path& path) const;
    std::string stamp(const std::filesystem::path& path) const;
};
}
