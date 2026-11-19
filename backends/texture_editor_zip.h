#pragma once
#include <filesystem>
#include <string>
#include <vector>

namespace RecompTextureZip {
struct Image {
    std::vector<uint8_t> pixels;
    int width = 0, height = 0;
};

std::vector<uint8_t> readPng(const std::filesystem::path&);
Image decodePng(const std::vector<uint8_t>&);
std::vector<uint8_t> encodePng(const Image&);

// Builds a sibling archive before publication so an interrupted write cannot
// damage the active pack. Call publish after closing RT64's archive readers.
class Edit {
public:
    Edit(const std::filesystem::path& zip, const std::string& entry, const std::vector<uint8_t>& png);
    ~Edit();
    Edit(const Edit&) = delete;
    Edit& operator=(const Edit&) = delete;
    void publish();

private:
    std::filesystem::path zip, directory;
    uintmax_t size;
    std::filesystem::file_time_type modified;
};
}
