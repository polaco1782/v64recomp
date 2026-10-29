#include "textures.h"
#include "texture_decode.h"
#include "xxHash/xxh3.h"
#include "common/rt64_tmem_hasher.h"
#include "common/rt64_filesystem_directory.h"
#include "stb/stb_image.h"
#define STBIW_WINDOWS_UTF8
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"
#include <cstdlib>
#include <fstream>
#include <regex>

namespace {
// RT64's upload wait covers TMEM uploads. A replacement-only batch can still
// be running, so finish that worker before rebuilding its directory/index.
struct PausedUploads {
    RT64::TextureCache& cache;
    explicit PausedUploads(RT64::TextureCache& cache) : cache(cache) {
        cache.waitForAllStreamThreads(false);
        cache.waitForGPUUploads();
        {
            // Change the wait predicate under the waiter's mutex. Otherwise
            // the notification can arrive between its predicate check and
            // sleep, leaving join() waiting forever for an idle uploader.
            std::unique_lock lock(cache.uploadQueueMutex);
            cache.uploadThreadRunning = false;
        }
        cache.uploadQueueChanged.notify_all();
        cache.uploadThread->join();
        cache.uploadThread.reset();
        std::unique_lock lock(cache.uploadQueueMutex);
        cache.resolvedPathQueue.clear();
    }
    ~PausedUploads() {
        cache.uploadThreadRunning = false;
        cache.uploadThread = std::make_unique<std::thread>(&RT64::TextureCache::uploadThreadLoop, &cache);
        // The loop sets this flag at entry. Wait for entry so a second F5
        // cannot request a stop before the new thread has started.
        while (!cache.uploadThreadRunning.load())
            std::this_thread::yield();
    }
};
}

void RecompTextureFiles::configure(RT64::TextureCache& cache, const std::string& title, size_t rdramSize) {
    romTitle = title;
    rdramByteCount = rdramSize;
#ifdef RECOMP_DEBUG
    if (const char *path = std::getenv("RECOMP_TEXTURE_DUMP"); path && *path) {
        dumpDirectory = path;
        std::filesystem::create_directories(dumpDirectory);
        std::fprintf(stderr, "Texture PNG dump: %s\n", dumpDirectory.u8string().c_str());
    }
#endif
    if (const char *path = std::getenv("RECOMP_TEXTURE_REPLACEMENTS"); path && *path)
        replacementDirectory = path;
    if (const char *path = std::getenv("RECOMP_TEXTURE_PACK"); path && *path) {
        replacementDirectory = path;
        zipPack = true;
    }
    reload(cache);
}

void RecompTextureFiles::reload(RT64::TextureCache& cache) {
    if (replacementDirectory.empty())
        return;
    replacementFiles = 0;
    replaced.clear();
    PausedUploads pause(cache);
    cache.clearReplacementDirectories();
    if (zipPack) {
#ifdef RECOMP_DEBUG
        constexpr bool logLoads = true;
#else
        constexpr bool logLoads = false;
#endif
        if (!cache.loadTexturePack(replacementDirectory, romTitle, rdramByteCount, logLoads))
            throw std::runtime_error("could not load texture ZIP");
        return;
    }
    // A missing pack is normal: the game uses its original textures.
    if (!std::filesystem::exists(replacementDirectory))
        return;
    const auto scannedPack = replacementDirectory / "textures.rtz";
    const auto loadPath =
        std::filesystem::is_directory(replacementDirectory) && std::filesystem::is_regular_file(scannedPack)
            ? scannedPack
            : replacementDirectory;
    if (std::filesystem::is_regular_file(loadPath) ||
        std::filesystem::is_regular_file(replacementDirectory / "rt64.json")) {
        // Standard RT64 packs retain their own streaming, mip cache, hash
        // version, and UV shift settings. Uploads stay paused while the index
        // is replaced; PausedUploads restarts the worker on return.
        if (!cache.loadReplacementDirectory(RT64::ReplacementDirectory(loadPath)))
            throw std::runtime_error("could not load RT64 texture pack");
        {
            std::unique_lock lock(cache.textureMapMutex);
            for (const auto& paths : cache.textureMap.replacementMap.fileSystemResolvedPaths)
                replacementFiles += paths.size();
        }
        std::fprintf(stderr, "Texture pack: %llu mappings in %s (F5 reloads and toggles originals/replacements)\n",
                     static_cast<unsigned long long>(replacementFiles), loadPath.u8string().c_str());
        return;
    }
    if (!std::filesystem::is_directory(replacementDirectory))
        throw std::runtime_error("texture replacements must be a directory");
    RT64::ReplacementDatabase db;
    db.config.hashVersion = RT64::TMEMHasher::CurrentHashVersion;
    db.config.defaultOperation = RT64::ReplacementOperation::Stall;
    const std::regex name("([0-9a-fA-F]{16})\\.v" + std::to_string(db.config.hashVersion) + "\\.png");
    for (const auto& entry : std::filesystem::directory_iterator(replacementDirectory)) {
        std::smatch match;
        std::string filename = entry.path().filename().string();
        if (!entry.is_regular_file() || !std::regex_match(filename, match, name))
            continue;
        int w = 0, h = 0, channels = 0;
#ifdef _WIN32
        FILE *png = _wfopen(entry.path().c_str(), L"rb");
#else
        FILE *png = std::fopen(entry.path().c_str(), "rb");
#endif
        bool valid = png && stbi_info_from_file(png, &w, &h, &channels);
        if (png)
            std::fclose(png);
        if (!valid || w <= 0 || h <= 0 || w > 8192 || h > 8192) {
            std::fprintf(stderr, "Ignoring invalid replacement PNG: %s\n", entry.path().u8string().c_str());
            continue;
        }
        RT64::ReplacementTexture texture;
        texture.hashes.rt64 = match[1].str();
        texture.path = filename;
        db.addReplacement(texture);
    }
    // Use RT64's resolver and GPU upload path, with an in-memory index so a
    // directory of hash-named PNGs requires no generated rt64.json file.
    {
        std::unique_lock lock(cache.textureMapMutex);
        auto& map = cache.textureMap.replacementMap;
        map.replacementDirectories = {RT64::ReplacementDirectory(replacementDirectory)};
        map.fileSystemIsDirectory = true;
        map.fileSystems.clear();
        map.fileSystems.emplace_back(RT64::FileSystemDirectory::create(replacementDirectory));
        map.fileSystemResolvedPaths.clear();
        map.fileSystemResolvedPaths.resize(1);
        map.fileSystemHashVersions = {db.config.hashVersion};
        map.fileSystemStreamSets.resize(1);
        map.fileSystemStreamResolvedPaths.resize(1);
        map.directoryDatabase = db;
        db.resolvePaths(map.fileSystems[0].get(), 0, map.fileSystemResolvedPaths[0], false);
        map.resolvedHashVersions = {db.config.hashVersion};
        replacementFiles = map.fileSystemResolvedPaths[0].size();
    }
    cache.reloadReplacements(true);
    std::fprintf(stderr, "Texture replacements: %llu PNGs in %s (F5 reloads and toggles originals/replacements)\n",
                 static_cast<unsigned long long>(replacementFiles), replacementDirectory.u8string().c_str());
}

void RecompTextureFiles::refreshAndToggle(RT64::TextureCache& cache) {
    if (replacementDirectory.empty())
        return;
    reload(cache);
    std::unique_lock lock(cache.textureMapMutex);
    auto& map = cache.textureMap;
    map.replacementMapEnabled = !map.replacementMapEnabled;
    // Refresh descriptors as well as UV scaling for textures already in use.
    for (size_t i = 0; i < map.textureReplacements.size(); i++) {
        if (map.textureReplacements[i])
            map.versions[i]++;
    }
    map.globalVersion++;
    std::fprintf(stderr, "Textures: %s\n", map.replacementMapEnabled ? "replacements" : "originals");
}

void RecompTextureFiles::collect(RT64::TextureCache& cache) {
#ifdef RECOMP_DEBUG
    if (dumpDirectory.empty() && !replacementFiles && !zipPack)
        return;
    struct Pending {
        uint64_t hash;
        RT64::Texture texture;
    };
    std::vector<Pending> pending;
    {
        std::unique_lock lock(cache.textureMapMutex);
        if (zipPack) {
            replacementFiles = 0;
            for (const auto& paths : cache.textureMap.replacementMap.fileSystemResolvedPaths)
                replacementFiles += paths.size();
        }
        for (const auto& [hash, index] : cache.textureMap.hashMap) {
            if (cache.textureMap.replacementMapEnabled && cache.textureMap.textureReplacements[index])
                replaced.insert(hash);
            if (dumpDirectory.empty() || dumped.count(hash))
                continue;
            const auto *source = cache.textureMap.textures[index];
            if (!source || !source->decodeTMEM || source->bytesTMEM.size() != 4096)
                continue;
            Pending item{};
            item.hash = hash;
            item.texture.width = source->width;
            item.texture.height = source->height;
            item.texture.loadTile = source->loadTile;
            item.texture.tlut = source->tlut;
            item.texture.bytesTMEM = source->bytesTMEM;
            pending.emplace_back(std::move(item));
        }
    }
    for (const auto& item : pending) {
        const auto& texture = item.texture;
        const auto& tile = texture.loadTile;
        std::string base = RT64::ReplacementDatabase::hashToString(item.hash) + ".v" +
                           std::to_string(RT64::TMEMHasher::CurrentHashVersion);
        auto png = dumpDirectory / (base + ".png"), metadata = dumpDirectory / (base + ".json");
        if (std::filesystem::exists(png) && std::filesystem::exists(metadata)) {
            dumped.insert(item.hash);
            continue;
        }
        std::vector<uint8_t> pixels;
        try {
            pixels = RecompTextures::decode(texture.bytesTMEM.data(), texture.width, texture.height, tile.fmt, tile.siz,
                                            tile.tmem, tile.line, tile.palette, texture.tlut);
        } catch (const std::invalid_argument& e) {
            std::fprintf(stderr, "Skipping texture %s: %s\n", base.c_str(), e.what());
            dumped.insert(item.hash);
            continue;
        }
        auto temporary = dumpDirectory / (base + ".png.tmp");
        if (!stbi_write_png(temporary.u8string().c_str(), texture.width, texture.height, 4, pixels.data(), texture.width * 4))
            throw std::runtime_error("could not write texture PNG");
        // Publish the complete PNG before its metadata so a complete pair
        // always contains a fully written image.
        std::filesystem::rename(temporary, png);
        json data{{"hash", RT64::ReplacementDatabase::hashToString(item.hash)},
                  {"hash_version", uint32_t(RT64::TMEMHasher::CurrentHashVersion)},
                  {"width", texture.width},
                  {"height", texture.height},
                  {"tile", tile},
                  {"tlut", texture.tlut}};
        bool transparent = false;
        for (size_t p = 3; p < pixels.size(); p += 4)
            transparent |= pixels[p] < 255;
        data["transparent"] = transparent;
        auto metadataTemp = dumpDirectory / (base + ".json.tmp");
        std::ofstream stream(metadataTemp);
        stream << data.dump(2) << '\n';
        stream.close();
        if (!stream)
            throw std::runtime_error("could not write texture metadata");
        std::filesystem::rename(metadataTemp, metadata);
        dumped.insert(item.hash);
        pngsWritten++;

        std::fprintf(stderr, "Dumped texture %s: %ux%u, TLUT %u, %s\n", base.c_str(), texture.width, texture.height,
                     texture.tlut, transparent ? "transparent" : "opaque");
    }
#else
    (void)cache;
#endif
}
