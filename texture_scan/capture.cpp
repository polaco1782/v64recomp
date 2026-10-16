#include "capture.h"

#include <algorithm>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <cassert>
#include <sys/resource.h>
#include "shared/rt64_f3d_defines.h"
#include "xxHash/xxh3.h"
#include "hle/rt64_rdp.h"
#include "common/rt64_tmem_hasher.h"

namespace fs = std::filesystem;
namespace RecompTextures {
namespace {
constexpr size_t maxCaptureBytes = 1024 * 1024, maxQueuedBytes = 8 * 1024 * 1024;
constexpr size_t maxTaskBytes = 256 * 1024, maxTaskCaptures = 8;
struct Range {
    uint64_t start, count;
    bool valid(size_t size) const {
        return count && count <= maxCaptureBytes && start < size && count <= size - start;
    }
};
Range textureRange(const RT64::LoadOperation& load, const RT64::LoadTile& tile, uint16_t width, uint16_t height) {
    uint64_t offset = uint64_t(load.tile.uls >> 2) << load.texture.siz >> 1;
    uint64_t rowBytes = uint64_t(load.texture.width) << load.texture.siz >> 1;
    Range range{load.texture.address, 0};
    if (load.type == RT64::LoadOperation::Type::Block) {
        range.start += offset + rowBytes * load.tile.ult;
        range.count = uint64_t(((load.tile.lrs - load.tile.uls) >> (4 - load.tile.siz)) + 1) << 3;
        rowBytes = std::max(rowBytes, uint64_t(tile.line) << 3);
    } else if (load.type == RT64::LoadOperation::Type::Tile) {
        range.start += offset + rowBytes * (load.tile.ult >> 2);
        range.count = uint64_t(1 + ((load.tile.lrt >> 2) - (load.tile.ult >> 2))) * rowBytes;
    }
    range.count = std::max(range.count, std::max(uint64_t(width) << tile.siz >> 1, rowBytes) * height);
    return range;
}
Range paletteRange(const RT64::LoadOperation& load) {
    uint64_t rowBytes = uint64_t(load.texture.width) << load.texture.siz >> 1;
    uint64_t rows = 1 + ((load.tile.lrt >> 2) - (load.tile.ult >> 2));
    uint64_t words = ((load.tile.lrs >> 2) - (load.tile.uls >> 2)) + 1;
    return {load.texture.address + (uint64_t(load.tile.uls >> 2) << load.texture.siz >> 1) +
                rowBytes * (load.tile.ult >> 2),
            (rows - 1) * rowBytes + (words << 3)};
}
void publish(const fs::path& path, const void *bytes, size_t size) {
    auto temporary = path;
    temporary += ".scan-tmp";
    std::ofstream output(temporary, std::ios::binary);
    output.write(static_cast<const char *>(bytes), size);
    output.close();
    if (!output)
        throw std::runtime_error("cannot write " + temporary.string());
    fs::rename(temporary, path);
}
void publishJson(const fs::path& path, const json& data) {
    auto bytes = data.dump(2);
    publish(path, bytes.data(), bytes.size());
}
json operation(const RT64::LoadOperation& load) {
    return {{"tile", load.tile}, {"type", load.type}, {"texture", load.texture}};
}
}
CaptureWriter::CaptureWriter(fs::path path) : directory(std::move(path)) {
    fs::create_directories(directory);
    worker = std::thread(&CaptureWriter::run, this);
}
CaptureWriter::~CaptureWriter() {
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    changed.notify_one();
    worker.join();
}
void CaptureWriter::beginTask() {
    taskBytes = taskCount = 0;
}
bool CaptureWriter::capture(uint64_t hash, const uint8_t *tmem, const uint8_t *rdram, size_t rdramSize,
                            const RT64::LoadTile& tile, const RT64::LoadOperation& load,
                            const RT64::LoadOperation& palette, uint16_t width, uint16_t height, uint32_t tlut) {
    if (taskCount >= maxTaskCaptures || taskBytes >= maxTaskBytes)
        return false;
    // RT64 records Rice load operations at the start of a TMEM load. Interior
    // mip levels may have no record at all. Do not capture guest address zero
    // from that default record; allow a later draw with real load info to retry.
    if (!load.texture.width || (tlut && !palette.texture.width))
        return false;
    // Decline invalid captures without repeatedly attempting unsafe memory reads.
    if (!width || !height || tile.siz > 3 || load.texture.siz > 3 || load.tile.siz > 3 ||
        (load.type != RT64::LoadOperation::Type::Tile && load.type != RT64::LoadOperation::Type::Block) ||
        (tlut && palette.texture.siz > 3))
        return true;
    auto texture = textureRange(load, tile, width, height);
    auto colors = tlut ? paletteRange(palette) : Range{0, 0};
    if (!texture.valid(rdramSize) || (tlut && !colors.valid(rdramSize)))
        return true;
    size_t bytes = 4096 + texture.count + colors.count;
    std::unique_lock lock(mutex, std::try_to_lock);
    if (!lock || stopping || queuedBytes + bytes > maxQueuedBytes || queue.size() >= 64)
        return false;
    Capture capture{hash, tile, load, palette, width, height, tlut, {}, {}, {}};
    std::memcpy(capture.tmem.data(), tmem, capture.tmem.size());
    capture.rdram.assign(rdram + texture.start, rdram + texture.start + texture.count);
    if (tlut)
        capture.palette.assign(rdram + colors.start, rdram + colors.start + colors.count);
    queue.push_back(std::move(capture));
    queuedBytes += bytes;
    taskBytes += bytes;
    taskCount++;
    lock.unlock();
    changed.notify_one();
    return true;
}
void CaptureWriter::write(const Capture& capture) {
    char base[64];
    std::snprintf(base, sizeof(base), "%016llx.v%u", static_cast<unsigned long long>(capture.hash),
                  RT64::TMEMHasher::CurrentHashVersion);
    auto path = [&](const char *extension) { return directory / (std::string(base) + extension); };
    // Completed captures are immutable and can be reused from earlier launches.
    if (fs::exists(path(".tile.json")))
        return;
    publish(path(".tmem"), capture.tmem.data(), capture.tmem.size());
    publish(path(".rice.rdram"), capture.rdram.data(), capture.rdram.size());
    publishJson(path(".rice.json"), operation(capture.load));
    if (capture.tlut) {
        publish(path(".rice.palette.rdram"), capture.palette.data(), capture.palette.size());
        publishJson(path(".rice.palette.json"), operation(capture.paletteLoad));
    }
    auto tlut = capture.tlut == G_TT_RGBA16 ? RT64::LoadTLUT::RGBA16
                : capture.tlut == G_TT_IA16 ? RT64::LoadTLUT::IA16
                                            : RT64::LoadTLUT::None;
    // This atomic rename is the completion marker. The scanner needs no graphics lock.
    publishJson(path(".tile.json"),
                {{"tile", capture.tile}, {"width", capture.width}, {"height", capture.height}, {"tlut", tlut}});
}
void CaptureWriter::run() {
    setpriority(PRIO_PROCESS, 0, 10);
    for (;;) {
        Capture capture;
        {
            std::unique_lock lock(mutex);
            changed.wait(lock, [this] { return stopping || !queue.empty(); });
            if (queue.empty())
                return;
            capture = std::move(queue.front());
            queue.pop_front();
            queuedBytes -= capture.tmem.size() + capture.rdram.size() + capture.palette.size();
        }
        try {
            write(capture);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "Texture capture: %s\n", error.what());
        }
    }
}
}
