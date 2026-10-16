#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace RecompTextures {
// A scanner-owned archive transaction. Existing entries are copied without
// decoding; changed PNGs are written one at a time. Publication uses rename.
class Pack {
    struct Impl;
    std::unique_ptr<Impl> impl;

public:
    explicit Pack(const std::filesystem::path& directory);
    ~Pack();
    bool contains(const std::string& name) const;
    std::string stamp(const std::string& name) const;
    std::vector<uint8_t> read(const std::string& name) const;
    void put(const std::string& name, const std::vector<uint8_t>& bytes);
    bool finish();
    void cleanLooseFiles();
};
}
