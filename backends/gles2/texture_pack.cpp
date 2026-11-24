#include "texture_pack.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <cstdio>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <regex>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#ifdef _MSC_VER
#include <intrin.h>
#endif
#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"
#include "common/rt64_rice_hasher.h"
#include "shared/rt64_f3d_defines.h"
#include "common/rt64_tmem_hasher.h"
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz/miniz.h"
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb/stb_image.h"
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace fs = std::filesystem;
namespace {
constexpr unsigned maxFiles = 250000;
constexpr size_t maxPngBytes = GLES2_TEXTURE_MAX_PNG_BYTES;
constexpr unsigned maxDimension = 2048;
constexpr size_t maxSidecarBytes = 8 * 1024 * 1024;
constexpr unsigned hashVersion = 5;
static_assert(RT64::TMEMHasher::CurrentHashVersion == hashVersion,
              "Review the GLES2 texture compatibility before updating the RT64 version.");

struct Zip {
    mz_zip_archive archive{};
    FILE *file = nullptr;
    ~Zip() {
        if (archive.m_pState)
            mz_zip_reader_end(&archive);
        if (file)
            std::fclose(file);
    }
};

struct Candidate {
    mz_uint index;
    unsigned priority;
    mz_uint alpha = UINT32_MAX;
    bool opaque = false;
    bool ambiguous = false;
};

bool parseId(std::string_view name, uint64_t& id) {
    if (name.size() != 16)
        return false;
    auto result = std::from_chars(name.data(), name.data() + name.size(), id, 16);
    return result.ec == std::errc{} && result.ptr == name.data() + name.size();
}

std::string filename(uint64_t id) {
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.v5.png", static_cast<unsigned long long>(id));
    return name;
}

void add(std::unordered_map<uint64_t, Candidate>& out, uint64_t id, Candidate candidate) {
    auto [it, inserted] = out.emplace(id, candidate);
    if (!inserted) {
        if (candidate.priority > it->second.priority)
            it->second = candidate;
        else if (candidate.priority == it->second.priority &&
                 (candidate.index != it->second.index || candidate.alpha != it->second.alpha ||
                  candidate.opaque != it->second.opaque))
            it->second.ambiguous = true;
    }
}

std::string lowerAscii(std::string input) {
    std::transform(input.begin(), input.end(), input.begin(),
                   [](char c) { return c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : c; });
    return input;
}

bool endsWith(std::string_view string, std::string_view end) {
    return string.size() >= end.size() && string.substr(string.size() - end.size()) == end;
}

std::string riceMaskKey(const std::string& name) {
    size_t start = name.find_last_of('/') + 1;
    return name.substr(0, start) + lowerAscii(name.substr(start));
}

// RiceVideo / GLideN64 pack rules from RT64's live ZIP scanner. Keep miniz
// indices here so live matches use the same bounded decoder as v5 entries.
using RiceIndex = std::unordered_map<std::string, Candidate>;

RiceIndex indexRice(const std::unordered_map<std::string, mz_uint>& paths, const char *title) {
    static const std::regex pattern(
        R"(([^#]+)#(([0-9a-f]{8}|\$)#[0-4]#[0-3](?:#(?:[0-9a-f]{8}|\$))?)_(all|cibyrgba|allcibyrgba|rgb|a)\.png)");
    static const std::regex alternate(R"(^(?:(?:copy of|flash)\s+)+)");
    struct Parts {
        std::string key;
        std::optional<mz_uint> combined, rgb, alpha;
        unsigned combinedRank = 0;
        bool alternate = false, edited = false;
    };
    std::unordered_map<std::string, Parts> groups;
    const auto rom = lowerAscii(title ? title : "");
    for (const auto& [path, index] : paths) {
        const auto start = path.find_last_of('/') + 1;
        const auto name = lowerAscii(path.substr(start));
        std::smatch match;
        if (!std::regex_match(name, match, pattern))
            continue;
        const std::string prefix = match[1];
        if (std::regex_replace(prefix, alternate, "") != rom)
            continue;
        auto& parts = groups[path.substr(0, start) + name.substr(0, name.find_last_of('_'))];
        parts.key = match[2];
        parts.alternate = prefix != rom;
        parts.edited = path.substr(0, start) == "v64recomp/overrides/rice/";
        const std::string kind = match[4];
        std::optional<mz_uint> *slot = nullptr;
        if (kind == "rgb")
            slot = &parts.rgb;
        else if (kind == "a")
            slot = &parts.alpha;
        else {
            unsigned rank = kind == "all" ? 3 : kind == "cibyrgba" ? 2 : 1;
            if (rank < parts.combinedRank)
                continue;
            if (rank > parts.combinedRank)
                parts.combined.reset();
            parts.combinedRank = rank;
            slot = &parts.combined;
        }
        *slot = slot->has_value() ? UINT32_MAX : index;
    }
    RiceIndex result;
    for (const auto& [stem, part] : groups) {
        const bool pair = part.rgb && part.alpha;
        const bool rgb = pair || !part.combined;
        const auto index = rgb ? part.rgb : part.combined;
        if (!index)
            continue;
        const unsigned rank = (part.edited ? 8 : 0) + (part.alternate ? 0 : 4) + (pair ? 3 : rgb ? 1 : 2);
        const bool ambiguous = *index == UINT32_MAX || (pair && *part.alpha == UINT32_MAX);
        Candidate candidate{*index, rank, pair ? *part.alpha : UINT32_MAX, rgb && !pair, ambiguous};
        auto [it, inserted] = result.emplace(part.key, candidate);
        if (!inserted) {
            if (rank > it->second.priority)
                it->second = candidate;
            else if (rank == it->second.priority)
                it->second.ambiguous = true;
        }
    }
    return result;
}

const Candidate *findRice(const RiceIndex& images, const std::string& glide, const std::string& dma) {
    for (const auto *key : {&glide, &dma}) {
        auto it = images.find(*key);
        if (it != images.end())
            return &it->second;
    }
    // Try both exact palette representations before a wildcard can claim the texture.
    for (const auto *key : {&glide, &dma}) {
        if (key->size() != 21 || key->compare(8, 3, "#2#") != 0)
            continue;
        auto it = images.find(key->substr(0, 13) + "$");
        if (it != images.end())
            return &it->second;
        it = images.find("$" + key->substr(8));
        if (it != images.end())
            return &it->second;
    }
    return nullptr;
}

std::string archiveName(mz_zip_archive& archive, mz_uint index) {
    unsigned length = mz_zip_reader_get_filename(&archive, index, nullptr, 0);
    if (length < 2 || length > 4096)
        return {};
    std::string result(length, '\0');
    if (!mz_zip_reader_get_filename(&archive, index, result.data(), length))
        return {};
    result.pop_back();
    if (result.find('\0') != std::string::npos)
        return {};
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

void cachedRice(const fs::path& zipPath, const char *romTitle, const std::unordered_map<std::string, mz_uint>& paths,
                std::unordered_map<uint64_t, Candidate>& entries) {
    fs::path sidecar = zipPath;
    sidecar += ".rt64.json";
    std::error_code error;
    auto size = fs::file_size(sidecar, error);
    if (error || !size || size > maxSidecarBytes || !romTitle || !*romTitle)
        return;
    try {
        const auto stamp = std::to_string(fs::file_size(zipPath)) + ":" +
                           std::to_string(fs::last_write_time(zipPath).time_since_epoch().count());
        std::ifstream stream(sidecar);
        const auto source = json::parse(stream);
        if (source.at("source").at("stamp") != stamp || source.at("source").at("romTitle") != romTitle ||
            source.at("configuration").at("hashVersion") != hashVersion || !source.at("textures").is_array())
            return;
        std::unordered_map<std::string, mz_uint> masks;
        for (const auto& [path, index] : paths) {
            if (!endsWith(lowerAscii(path.substr(path.find_last_of('/') + 1)), "_a.png"))
                continue;
            auto [it, inserted] = masks.emplace(riceMaskKey(path), index);
            if (!inserted)
                it->second = UINT32_MAX;
        }
        for (const auto& texture : source.at("textures")) {
            if (!texture.is_object() || !texture.contains("path") || !texture["path"].is_string() ||
                !texture.contains("hashes") || !texture["hashes"].is_object() || !texture["hashes"].contains("rt64") ||
                !texture["hashes"]["rt64"].is_string())
                continue;
            auto name = texture["path"].get<std::string>();
            const auto suffix = lowerAscii(name.substr(name.find_last_of('/') + 1));
            uint64_t id;
            const auto found = paths.find(name);
            if (!endsWith(suffix, ".png") || endsWith(suffix, "_a.png") || found == paths.end() ||
                found->second == UINT32_MAX || !parseId(texture["hashes"]["rt64"].get<std::string>(), id))
                continue;
            Candidate candidate{found->second, 1};
            if (endsWith(suffix, "_rgb.png")) {
                const auto counterpart = riceMaskKey(name.substr(0, name.size() - 8) + "_a.png");
                const auto alpha = masks.find(counterpart);
                if (alpha == masks.end())
                    candidate.opaque = true;
                else if (alpha->second == UINT32_MAX)
                    continue; // A case-insensitive match with several masks is ambiguous.
                else
                    candidate.alpha = alpha->second;
            }
            add(entries, id, candidate);
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "GLES2 ignoring RT64 texture sidecar: %s\n", e.what());
    }
}

}

bool gles2_texture_decode_png(const uint8_t *bytes, size_t count, unsigned width, unsigned height, unsigned limit,
                              Gles2PackImage *image) {
    constexpr std::array<uint8_t, 8> png{137, 80, 78, 71, 13, 10, 26, 10};
    int w, h, channels;
    *image = {};
    limit = std::min(limit, maxDimension);
    if (!width || !height || !bytes || count < png.size() || count > maxPngBytes ||
        !std::equal(png.begin(), png.end(), bytes) || !stbi_info_from_memory(bytes, int(count), &w, &h, &channels) ||
        w <= 0 || h <= 0 || unsigned(w) > limit || unsigned(h) > limit || uint64_t(w) * height != uint64_t(h) * width)
        return false;
    image->pixels = stbi_load_from_memory(bytes, int(count), &w, &h, &channels, 4);
    if (!image->pixels)
        return false;
    image->width = unsigned(w);
    image->height = unsigned(h);
    return true;
}

struct RiceLoad {
    RT64::LoadOperation op{};
    std::vector<uint8_t> bytes;
    uint64_t serial = 0;
    unsigned rowBytes = 0;
};
struct RicePalette {
    std::array<uint8_t, 512> bytes{};
    std::array<uint64_t, 256> loads{};

    void capture(unsigned dest, unsigned count, uint64_t serial, const uint8_t *ram, size_t size, uint64_t src) {
        if (dest < 256 || dest >= 512)
            return;
        count = std::min(count, 512 - dest);
        if (src > size || count * 2 > size - src)
            return;
        std::memcpy(bytes.data() + (dest - 256) * 2, ram + src, count * 2);
        std::fill_n(loads.begin() + (dest - 256), count, serial);
    }

    std::vector<uint8_t> bank(const Tile& tile, const Gles2RiceWord *words) const {
        unsigned first = tile.siz == 0 ? tile.palette * 16 : 0;
        unsigned end = first;
        unsigned limit = tile.siz == 0 ? first + 16 : 256;
        while (end < limit && loads[end] && loads[end] == words[256 + end].load && words[256 + end].nibbles == 0xffff)
            ++end;
        return {bytes.begin() + first * 2, bytes.begin() + end * 2};
    }
};
// Each slot owns at most one decoded 2048x2048 RGBA image. Only the worker
// reads the ZIP after indexing. No queued request borrows TMEM or guest RAM.
struct DecodeRequest {
    enum State { Empty, Queued, Working, Finished } state = Empty;
    Gles2PackTile key{};
    uint64_t sequence = 0;
    unsigned limit = 0;
    bool local = false, live = false, missingLoad = false;
    std::optional<Candidate> candidate;
    Gles2PackImage image{};
};
struct Gles2TexturePack {
    Zip zip;
    RiceIndex rice;
    RicePalette dmaPalette, glidePalette;
    std::unique_ptr<RiceLoad[]> loads;
    size_t liveMatches = 0;
    fs::path overrides;
    std::unordered_set<uint64_t> localIds;
    std::unordered_map<uint64_t, Candidate> entries;
    std::array<DecodeRequest, 8> requests{};
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    uint64_t sequence = 0;
    bool stopping = false;

    void run();
    ~Gles2TexturePack() {
        if (worker.joinable()) {
            {
                std::lock_guard<std::mutex> lock(mutex);
                stopping = true;
            }
            wake.notify_one();
            worker.join();
        }
        for (auto& request : requests)
            gles2_texture_pack_free_image(&request.image);
    }
};

Gles2TexturePack *gles2_texture_pack_open(const char *path, const char *overrides, const char *romTitle) {
    if ((!path || !*path) && (!overrides || !*overrides))
        return nullptr;
    try {
        auto pack = std::make_unique<Gles2TexturePack>();
        if (overrides && *overrides) {
            std::error_code error;
            auto directory = fs::u8path(overrides);
            if (fs::is_directory(directory, error)) {
                // No extra identity work for games without a pack or any overrides.
                for (const auto& entry : fs::directory_iterator(directory, error)) {
                    const auto name = entry.path().filename().u8string();
                    uint64_t id;
                    if (name.size() == 23 && name.compare(16, 7, ".v5.png") == 0 &&
                        parseId(std::string_view(name).substr(0, 16), id)) {
                        pack->overrides = directory;
                        pack->localIds.insert(id);
                    }
                }
            }
        }
        if (!path || !*path)
            return pack.release();
        const auto zipPath = fs::u8path(path);
#ifdef _WIN32
        pack->zip.file = _wfopen(zipPath.c_str(), L"rb");
#else
        pack->zip.file = std::fopen(zipPath.c_str(), "rb");
#endif
        auto& archive = pack->zip.archive;
        if (!pack->zip.file || !mz_zip_reader_init_cfile(&archive, pack->zip.file, 0, 0) ||
            mz_zip_reader_get_num_files(&archive) > maxFiles) {
            std::fprintf(stderr, "GLES2 could not open texture ZIP: %s\n", path);
            return pack.release();
        }
        std::unordered_map<std::string, mz_uint> paths;
        for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&archive); ++i) {
            auto name = archiveName(archive, i);
            if (name.size() < 4 || name.find_first_of("#.") == std::string::npos)
                continue;
            auto [it, inserted] = paths.emplace(name, i);
            if (!inserted)
                it->second = UINT32_MAX;
            uint64_t id;
            constexpr std::string_view editorPrefix = "v64recomp/overrides/rt64-v5/";
            auto base = std::string_view(name).substr(name.find_last_of('/') + 1);
            if (name.size() == editorPrefix.size() + 20 &&
                std::string_view(name).compare(0, editorPrefix.size(), editorPrefix) == 0 &&
                base.substr(16) == ".png" && parseId(base.substr(0, 16), id))
                add(pack->entries, id, {i, 3});
            else if (base.size() == 23 && base.substr(16) == ".v5.png" && parseId(base.substr(0, 16), id))
                add(pack->entries, id, {i, 2});
        }
        pack->rice = indexRice(paths, romTitle);
        if (!pack->rice.empty())
            pack->loads = std::make_unique<RiceLoad[]>(512);
        cachedRice(zipPath, romTitle, paths, pack->entries);
        std::fprintf(stderr, "GLES2 texture ZIP: %zu versioned entries, %zu Rice IDs\n", pack->entries.size(),
                     pack->rice.size());
        return pack.release();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "GLES2 ignoring texture pack: %s\n", e.what());
        return nullptr;
    }
}

void gles2_texture_pack_close(Gles2TexturePack *pack) {
    if (pack && pack->liveMatches)
        std::fprintf(stderr, "GLES2 texture ZIP: %zu live Rice mappings decoded\n", pack->liveMatches);
    delete pack;
}

bool gles2_texture_pack_enabled(const Gles2TexturePack *pack) {
    return pack && (!pack->overrides.empty() || !pack->entries.empty() || !pack->rice.empty());
}

bool gles2_texture_pack_has_rice(const Gles2TexturePack *pack) {
    return pack && !pack->rice.empty();
}

static RT64::LoadTile loadTile(const Tile& t) {
    return {uint8_t(t.fmt), uint8_t(t.siz), uint16_t(t.line), uint16_t(t.tmem), uint8_t(t.palette),
            uint8_t(t.cs),  uint8_t(t.ct),  uint8_t(t.ms),    uint8_t(t.mt),    uint8_t(t.ss),
            uint8_t(t.st),  uint16_t(t.sl), uint16_t(t.tl),   uint16_t(t.sh),   uint16_t(t.th)};
}

void gles2_texture_pack_record_load(Gles2TexturePack *pack, uint64_t serial, const Tile *tile, uint32_t w0, uint32_t w1,
                                    unsigned imageWidth, unsigned imageSize, const TextureTransfer *dma,
                                    const uint8_t *ram, size_t ramSize, uint32_t address) {
    auto& record = pack->loads[tile->tmem];
    record.serial = 0;
    auto& op = record.op;
    const unsigned opcode = (w0 >> 24) | 0xc0;
    using Type = RT64::LoadOperation::Type;
    op.type = opcode == 0xf3 ? Type::Block : opcode == 0xf0 ? Type::TLUT : Type::Tile;
    op.tile = loadTile(*tile);
    op.tile.uls = (w0 >> 12) & 4095;
    op.tile.ult = w0 & 4095;
    op.tile.lrs = (w1 >> 12) & 4095;
    op.tile.lrt = w1 & 4095;
    op.texture = {address, 0, uint8_t(imageSize), uint16_t(imageWidth)};
    if (op.type == Type::TLUT) {
        // GLideN64's gDPLoadTLUT copies TexFilterPalette from SETTIMG's
        // base, even when the hardware load starts at a nonzero S/T offset.
        // Keep both conventions so an exact match outranks either wildcard.
        pack->glidePalette.capture(tile->tmem, dma->width * dma->rows, serial, ram, ramSize, address);
        for (unsigned y = 0; y < dma->rows; ++y) {
            unsigned dest = (tile->tmem + y * tile->line * 4) & 511;
            uint64_t offset = (dma->first_bit + uint64_t(y) * dma->stride_bits) / 8;
            pack->dmaPalette.capture(dest, dma->width, serial, ram, ramSize, address + offset);
        }
        return;
    }
    // Preserve the pinned hasher's raw word-swizzled RDRAM view before another
    // submission can reuse its pages. Only DMA source rows are available here.
    const uint64_t rowBytes = (dma->row_bits + (dma->first_bit & 7) + 7) / 8;
    const uint64_t stride = dma->stride_bits / 8;
    const uint64_t span = (dma->rows - 1) * stride + rowBytes;
    const uint64_t start = uint64_t(address) + dma->first_bit / 8;
    // Rice's block X offset uses quarter-texels. A nonmatching DMA origin
    // would require bytes outside this load, so leave it unmapped.
    const uint64_t riceStart =
        uint64_t(address) + ((uint64_t(op.tile.uls >> 2) << imageSize) >> 1) +
        ((uint64_t(imageWidth) << imageSize) >> 1) * (op.type == Type::Block ? op.tile.ult : (op.tile.ult >> 2));
    if (start != riceStart || !rowBytes || (dma->rows > 1 && (dma->stride_bits & 7)) || span > 64 * 1024 ||
        start > ramSize || span > ramSize - start)
        return;
    try {
        record.bytes.resize(span);
        if (dma->rows == 1 || stride == rowBytes)
            std::memcpy(record.bytes.data(), ram + start, span);
        else
            for (unsigned y = 0; y < dma->rows; ++y)
                std::memcpy(record.bytes.data() + y * stride, ram + start + y * stride, rowBytes);
        record.rowBytes = unsigned(rowBytes);
        record.serial = serial;
    } catch (const std::bad_alloc&) {
        // Optional replacements must not prevent the native texture from drawing.
    }
}

// Validate the sampled TMEM words against the captured DMA. A later partial
// overwrite or an interior mip level cannot borrow the base texture's Rice ID.
static const Candidate *liveRice(Gles2TexturePack& pack, const Tile& tile, unsigned tlut, const Gles2RiceWord *words,
                                 unsigned width, unsigned height, bool *missingLoad) {
    *missingLoad = true;
    const uint64_t serial = words[tile.tmem].load;
    if (!serial || (serial & 511) != tile.tmem)
        return nullptr;
    const auto& record = pack.loads[tile.tmem];
    if (record.serial != serial || record.op.type == RT64::LoadOperation::Type::TLUT)
        return nullptr;
    if (record.op.type == RT64::LoadOperation::Type::Tile) {
        // The checksum can reinterpret the texel size. Reject a wider row
        // before it reads the uncaptured gap between two strided DMA rows.
        const auto& load = record.op.tile;
        unsigned count = (1 + (load.lrs >> 2) - (load.uls >> 2)) & 1023;
        if (load.masks)
            count = std::min(count, 1u << load.masks);
        count = std::min(count, unsigned(record.op.texture.width));
        if ((count << std::max(tile.siz, unsigned(load.siz)) >> 1) > record.rowBytes)
            return nullptr;
    }
    const bool indexed = (tlut & 2) != 0;
    if (indexed && tile.siz > 1)
        return nullptr;
    const bool split = !indexed && tile.siz == 3;
    const unsigned mask = indexed || split ? 255 : 511;
    const unsigned rowNibbles = width << std::min(tile.siz, 2u);
    for (unsigned y = 0; y < height; ++y) {
        for (unsigned n = 0; n < rowNibbles; n += 16) {
            unsigned word = (tile.tmem + y * tile.line + n / 16) & mask;
            uint16_t covered = uint16_t((1u << std::min(16u, rowNibbles - n)) - 1);
            if (y & 1)
                covered = uint16_t((covered << 8) | (covered >> 8));
            auto owns = [&](unsigned i) { return words[i].load == serial && (words[i].nibbles & covered) == covered; };
            if (!owns(word) || (split && !owns(word + 256)))
                return nullptr;
        }
    }
    const auto draw = loadTile(tile);
    if (!indexed) {
        const auto key = RT64::calculateRiceHash(draw, record.op, RT64::LoadTLUT::None, record.bytes, {});
        *missingLoad = key.empty();
        return findRice(pack.rice, key, {});
    }
    const auto mode = tlut == 2 ? RT64::LoadTLUT::RGBA16 : RT64::LoadTLUT::IA16;
    auto glide = pack.glidePalette.bank(tile, words);
    auto dma = pack.dmaPalette.bank(tile, words);
    const auto glideKey =
        glide.empty() ? std::string{} : RT64::calculateRiceHash(draw, record.op, mode, record.bytes, glide);
    const auto dmaKey =
        dma.empty() || dma == glide ? std::string{} : RT64::calculateRiceHash(draw, record.op, mode, record.bytes, dma);
    *missingLoad = glideKey.empty() && dmaKey.empty();
    return findRice(pack.rice, glideKey, dmaKey);
}

bool gles2_texture_rt64_id(const uint8_t *tmem, unsigned format, unsigned size, unsigned line, unsigned address,
                           unsigned palette, unsigned tlut, unsigned width, unsigned height, uint64_t *id) {
    const bool indexed = format != G_IM_FMT_YUV && size <= G_IM_SIZ_8b && (tlut == 2 || tlut == 3);
    const bool ordinary =
        !tlut && ((format == G_IM_FMT_RGBA && (size == G_IM_SIZ_16b || size == G_IM_SIZ_32b)) ||
                  (format == G_IM_FMT_IA && size <= G_IM_SIZ_16b) || (format == G_IM_FMT_I && size <= G_IM_SIZ_8b));
    if (!line || !width || !height || width > 1024 || height > 1024 || palette > 15 ||
        (size == G_IM_SIZ_4b && width > 1 && (width & 1)) || (!indexed && !ordinary))
        return false;
    RT64::LoadTile tile{};
    tile.fmt = uint8_t(format);
    tile.siz = uint8_t(size);
    tile.line = uint16_t(line);
    tile.tmem = uint16_t(address);
    tile.palette = uint8_t(palette);
    const unsigned half = (indexed || size == G_IM_SIZ_32b) ? 2048 : 4096;
    const unsigned row = std::max((width << std::min(size, unsigned(G_IM_SIZ_16b))) >> 1, 1u);
    // Match ordinary RT64 decoded tiles only. The v5 row-tail swap assumes
    // an aligned 64-bit TMEM lane, and wraps exotic layouts differently.
    if (row > line * 8 || ((address * 8) & (half - 1)) + (height - 1) * (line * 8) + ((row + 7) & ~7u) > half ||
        RT64::TMEMHasher::requiresRawTMEM(tile, uint16_t(width), uint16_t(height)))
        return false;
    *id = RT64::TMEMHasher::hash(tmem, tile, uint16_t(width), uint16_t(height), tlut << G_MDSFT_TEXTLUT, hashVersion);
    return true;
}

static void decode(Gles2TexturePack& pack, DecodeRequest& job) {
    auto *image = &job.image;
    try {
        if (job.local) {
            const auto path = pack.overrides / filename(job.key.id);
            std::error_code error;
            auto size = fs::file_size(path, error);
            if (!error && size && size <= maxPngBytes) {
                std::vector<uint8_t> bytes(size_t(size), 0);
                std::ifstream stream(path, std::ios::binary);
                if (stream.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(size)) &&
                    gles2_texture_decode_png(bytes.data(), bytes.size(), job.key.width, job.key.height, job.limit,
                                             image)) {
                    job.live = false;
                    return;
                }
            }
        }
        if (!job.candidate)
            return;
        auto& archive = pack.zip.archive;
        auto decodeEntry = [&](mz_uint index, Gles2PackImage *out) {
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&archive, index, &stat) || !stat.m_uncomp_size ||
                stat.m_uncomp_size > maxPngBytes)
                return false;
            size_t count = 0;
            std::unique_ptr<void, decltype(&mz_free)> bytes(mz_zip_reader_extract_to_heap(&archive, index, &count, 0),
                                                            mz_free);
            return bytes && gles2_texture_decode_png(static_cast<const uint8_t *>(bytes.get()), count, job.key.width,
                                                     job.key.height, job.limit, out);
        };
        const auto candidate = *job.candidate;
        if (!decodeEntry(candidate.index, image))
            return;
        Gles2PackImage alpha{};
        if (candidate.alpha != UINT32_MAX &&
            (!decodeEntry(candidate.alpha, &alpha) || alpha.width != image->width || alpha.height != image->height)) {
            gles2_texture_pack_free_image(&alpha);
            gles2_texture_pack_free_image(image);
            return;
        }
        if (candidate.opaque || alpha.pixels)
            for (size_t i = 0; i < size_t(image->width) * image->height * 4; i += 4)
                image->pixels[i + 3] = alpha.pixels ? alpha.pixels[i] : 255;
        gles2_texture_pack_free_image(&alpha);
#ifndef NDEBUG
        if (job.live)
            std::fprintf(stderr, "GLES2 Rice loaded: %s (%ux%u)\n", archiveName(archive, candidate.index).c_str(),
                         image->width, image->height);
#endif
    } catch (const std::exception&) {
        gles2_texture_pack_free_image(image);
    }
}

void Gles2TexturePack::run() {
    std::unique_lock<std::mutex> lock(mutex);
    for (;;) {
        DecodeRequest *next = nullptr;
        for (auto& job : requests)
            if (job.state == DecodeRequest::Queued && (!next || job.sequence < next->sequence))
                next = &job;
        if (stopping)
            return;
        if (!next) {
            wake.wait(lock);
            continue;
        }
        next->state = DecodeRequest::Working;
        lock.unlock();
        decode(*this, *next);
        lock.lock();
        next->state = DecodeRequest::Finished;
    }
}

bool gles2_texture_pack_take(Gles2TexturePack *pack, Gles2PackTile *key, Gles2PackImage *image) {
    if (!pack || !pack->worker.joinable())
        return false;
    std::lock_guard<std::mutex> lock(pack->mutex);
    DecodeRequest *next = nullptr;
    for (auto& job : pack->requests)
        if (job.state == DecodeRequest::Finished && job.image.pixels && (!next || job.sequence < next->sequence))
            next = &job;
    if (!next)
        return false;
    if (next->live) {
        if (next->key.has_id) {
            try {
                pack->entries.emplace(next->key.id, *next->candidate);
            } catch (const std::bad_alloc&) {
                // The decoded image can still be uploaded without memoizing the Rice match.
            }
        }
        ++pack->liveMatches;
    }
    *key = next->key;
    *image = next->image;
    *next = {};
    return true;
}

Gles2PackStatus gles2_texture_pack_request(Gles2TexturePack *pack, Gles2PackTile key, const Tile *tile, unsigned tlut,
                                           const Gles2RiceWord words[512], unsigned limit) {
    try {
        if (!pack)
            return GLES2_PACK_MISS;
        std::unique_lock<std::mutex> lock(pack->mutex);
        DecodeRequest *slot = nullptr;
        for (auto& job : pack->requests) {
            if (job.state != DecodeRequest::Empty && job.key.hash == key.hash && job.key.has_id == key.has_id &&
                job.key.id == key.id) {
                if (job.state != DecodeRequest::Finished || job.image.pixels)
                    return GLES2_PACK_PENDING;
                bool incomplete = job.missingLoad;
                job = {};
                return incomplete ? GLES2_PACK_INCOMPLETE : GLES2_PACK_MISS;
            }
            if (job.state == DecodeRequest::Empty || (!slot && job.state == DecodeRequest::Finished && !job.image.pixels))
                slot = &job;
        }
        // Only the render thread enqueues or takes a finished slot. Release
        // the worker while looking up the original texture and palette.
        lock.unlock();
        const auto entry = key.has_id ? pack->entries.find(key.id) : pack->entries.end();
        const bool live = entry == pack->entries.end();
        bool missing = false;
        const Candidate *found =
            live ? (pack->loads ? liveRice(*pack, *tile, tlut, words, key.width, key.height, &missing) : nullptr)
                 : &entry->second;
        const bool local = key.has_id && pack->localIds.count(key.id);
        if (!local && (!found || found->ambiguous))
            return missing ? GLES2_PACK_INCOMPLETE : GLES2_PACK_MISS;
        if (!slot)
            return GLES2_PACK_PENDING;
        lock.lock();
        if (!pack->worker.joinable())
            pack->worker = std::thread(&Gles2TexturePack::run, pack);
        *slot = {};
        slot->state = DecodeRequest::Queued;
        slot->sequence = ++pack->sequence;
        slot->key = key;
        slot->local = local;
        slot->live = live;
        slot->missingLoad = missing;
        slot->limit = limit;
        if (found && !found->ambiguous)
            slot->candidate = *found;
        lock.unlock();
        pack->wake.notify_one();
        return GLES2_PACK_PENDING;
    } catch (const std::exception&) {
        return GLES2_PACK_MISS;
    }
}

void gles2_texture_pack_free_image(Gles2PackImage *image) {
    stbi_image_free(image->pixels);
    *image = {};
}
