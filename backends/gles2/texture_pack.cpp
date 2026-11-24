#include "texture_pack.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>
#include <unordered_map>

#ifdef _MSC_VER
#include <intrin.h>
#endif
#define XXH_INLINE_ALL
#include "xxHash/xxhash.h"
#include "common/rt64_load_types.h"
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

bool parseId(std::string_view name, uint64_t &id) {
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

void add(std::unordered_map<uint64_t, Candidate> &out, uint64_t id, Candidate candidate) {
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

std::string riceMaskKey(const std::string &name) {
    size_t start = name.find_last_of('/') + 1;
    return name.substr(0, start) + lowerAscii(name.substr(start));
}

std::string archiveName(mz_zip_archive &archive, mz_uint index) {
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

void cachedRice(const fs::path &zipPath, const char *romTitle,
                const std::unordered_map<std::string, mz_uint> &paths,
                std::unordered_map<uint64_t, Candidate> &entries) {
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
        for (const auto &[path, index] : paths) {
            if (!endsWith(lowerAscii(path.substr(path.find_last_of('/') + 1)), "_a.png"))
                continue;
            auto [it, inserted] = masks.emplace(riceMaskKey(path), index);
            if (!inserted)
                it->second = UINT32_MAX;
        }
        for (const auto &texture : source.at("textures")) {
            if (!texture.is_object() || !texture.contains("path") || !texture["path"].is_string() ||
                !texture.contains("hashes") || !texture["hashes"].is_object() ||
                !texture["hashes"].contains("rt64") || !texture["hashes"]["rt64"].is_string())
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
    } catch (const std::exception &e) {
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
        !std::equal(png.begin(), png.end(), bytes) ||
        !stbi_info_from_memory(bytes, int(count), &w, &h, &channels) || w <= 0 || h <= 0 ||
        unsigned(w) > limit || unsigned(h) > limit || uint64_t(w) * height != uint64_t(h) * width)
        return false;
    image->pixels = stbi_load_from_memory(bytes, int(count), &w, &h, &channels, 4);
    if (!image->pixels)
        return false;
    image->width = unsigned(w);
    image->height = unsigned(h);
    return true;
}

struct Gles2TexturePack {
    Zip zip;
    fs::path overrides;
    std::unordered_map<uint64_t, Candidate> entries;
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
                for (const auto &entry : fs::directory_iterator(directory, error)) {
                    const auto name = entry.path().filename().u8string();
                    uint64_t id;
                    if (name.size() == 23 && name.compare(16, 7, ".v5.png") == 0 &&
                        parseId(std::string_view(name).substr(0, 16), id)) {
                        pack->overrides = std::move(directory);
                        break;
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
        auto &archive = pack->zip.archive;
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
        cachedRice(zipPath, romTitle, paths, pack->entries);
        std::fprintf(stderr, "GLES2 texture ZIP: %zu versioned entries\n", pack->entries.size());
        return pack.release();
    } catch (const std::exception &e) {
        std::fprintf(stderr, "GLES2 ignoring texture pack: %s\n", e.what());
        return nullptr;
    }
}

void gles2_texture_pack_close(Gles2TexturePack *pack) {
    delete pack;
}

bool gles2_texture_pack_enabled(const Gles2TexturePack *pack) {
    return pack && (!pack->overrides.empty() || !pack->entries.empty());
}

bool gles2_texture_rt64_id(const uint8_t *tmem, unsigned format, unsigned size, unsigned line, unsigned address,
                           unsigned palette, unsigned tlut, unsigned width, unsigned height, uint64_t *id) {
    const bool indexed = format != G_IM_FMT_YUV && size <= G_IM_SIZ_8b && (tlut == 2 || tlut == 3);
    const bool ordinary = !tlut &&
                          ((format == G_IM_FMT_RGBA && (size == G_IM_SIZ_16b || size == G_IM_SIZ_32b)) ||
                           (format == G_IM_FMT_IA && size <= G_IM_SIZ_16b) ||
                           (format == G_IM_FMT_I && size <= G_IM_SIZ_8b));
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
    if (row > line * 8 || ((address * 8) & (half - 1)) + (height - 1) * (line * 8) +
                             ((row + 7) & ~7u) > half ||
        RT64::TMEMHasher::requiresRawTMEM(tile, uint16_t(width), uint16_t(height)))
        return false;
    *id = RT64::TMEMHasher::hash(tmem, tile, uint16_t(width), uint16_t(height), tlut << G_MDSFT_TEXTLUT, hashVersion);
    return true;
}

bool gles2_texture_pack_image(Gles2TexturePack *pack, uint64_t id, unsigned width, unsigned height, unsigned limit,
                              Gles2PackImage *image) {
    *image = {};
    if (!pack)
        return false;
    try {
        if (!pack->overrides.empty()) {
            const auto path = pack->overrides / filename(id);
            std::error_code error;
            auto size = fs::file_size(path, error);
            if (!error && size && size <= maxPngBytes) {
                std::vector<uint8_t> bytes(size_t(size), 0);
                std::ifstream stream(path, std::ios::binary);
                if (stream.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(size)) &&
                    gles2_texture_decode_png(bytes.data(), bytes.size(), width, height, limit, image))
                    return true;
            }
        }
        const auto entry = pack->entries.find(id);
        if (entry == pack->entries.end() || entry->second.ambiguous)
            return false;
        auto &archive = pack->zip.archive;
        auto decodeEntry = [&](mz_uint index, Gles2PackImage *out) {
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&archive, index, &stat) || !stat.m_uncomp_size ||
                stat.m_uncomp_size > maxPngBytes)
                return false;
            size_t count = 0;
            std::unique_ptr<void, decltype(&mz_free)> bytes(
                mz_zip_reader_extract_to_heap(&archive, index, &count, 0), mz_free);
            return bytes &&
                   gles2_texture_decode_png(static_cast<const uint8_t *>(bytes.get()), count, width, height, limit, out);
        };
        const auto &candidate = entry->second;
        if (!decodeEntry(candidate.index, image))
            return false;
        Gles2PackImage alpha{};
        if (candidate.alpha != UINT32_MAX &&
            (!decodeEntry(candidate.alpha, &alpha) || alpha.width != image->width || alpha.height != image->height)) {
            gles2_texture_pack_free_image(&alpha);
            gles2_texture_pack_free_image(image);
            return false;
        }
        if (candidate.opaque || alpha.pixels)
            for (size_t i = 0; i < size_t(image->width) * image->height * 4; i += 4)
                image->pixels[i + 3] = alpha.pixels ? alpha.pixels[i] : 255;
        gles2_texture_pack_free_image(&alpha);
        return true;
    } catch (const std::exception &) {
        return false;
    }
}

void gles2_texture_pack_free_image(Gles2PackImage *image) {
    stbi_image_free(image->pixels);
    *image = {};
}
