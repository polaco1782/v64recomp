#pragma once

#include "common/rt64_load_types.h"
#include <array>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace RecompTextures {
// Copies guest memory while it is stable, then writes it on a separate worker.
// The producer never waits for the writer, the filesystem, or the scanner.
class CaptureWriter {
    struct Capture {
        uint64_t hash;
        RT64::LoadTile tile;
        RT64::LoadOperation load, paletteLoad;
        uint16_t width, height;
        uint32_t tlut;
        std::array<uint8_t, 4096> tmem;
        std::vector<uint8_t> rdram, palette;
    };
    std::filesystem::path directory;
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Capture> queue;
    std::thread worker;
    bool stopping = false;
    size_t queuedBytes = 0, taskBytes = 0, taskCount = 0;
    void run();
    void write(const Capture& capture);

public:
    explicit CaptureWriter(std::filesystem::path directory);
    ~CaptureWriter();
    void beginTask(); // Graphics producer only.
    bool capture(uint64_t hash, const uint8_t *tmem, const uint8_t *rdram, size_t rdramSize, const RT64::LoadTile& tile,
                 const RT64::LoadOperation& load, const RT64::LoadOperation& palette, uint16_t width, uint16_t height,
                 uint32_t tlut);
};
}
