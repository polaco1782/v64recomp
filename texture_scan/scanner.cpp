#include "scanner.h"
#include "pack.h"
#include "source.h"
#include "tools/texture_hasher/texture_hasher.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <stdexcept>
#include <vector>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#include <sys/resource.h>
#include "json/json.hpp"
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace RecompTextures {
namespace {
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}
json readJson(const fs::path& path) {
    std::ifstream stream(path);
    if (!stream)
        throw std::runtime_error("cannot read " + path.string());
    json result;
    stream >> result;
    return result;
}
void writeJson(const fs::path& path, const json& data) {
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream stream(temporary);
        stream << data.dump(2) << '\n';
        stream.close();
        if (!stream)
            throw std::runtime_error("cannot write " + temporary.string());
    }
    fs::rename(temporary, path);
}
// .tile.json is atomically published last by the capture writer. A parsed tile plus all
// required files forms an immutable snapshot; unfinished dumps are retried.
size_t snapshot(const fs::path& raw, const fs::path& ready) {
    size_t added = 0;
    const std::string suffix = ".tile.json";
    for (const auto& file : fs::directory_iterator(raw)) {
        auto name = file.path().filename().string();
        if (!file.is_regular_file() || name.size() <= suffix.size() ||
            name.substr(name.size() - suffix.size()) != suffix)
            continue;
        if (fs::exists(ready / name))
            continue;
        try {
            auto tile = readJson(file.path());
            auto base = name.substr(0, name.size() - suffix.size());
            auto load = readJson(raw / (base + ".rice.json"));
            // Old captures can contain an unset Rice record for a mip level.
            // It has no source image and must never be hashed from RDRAM zero.
            if (!load.at("texture").value("width", 0))
                continue;
            std::vector<std::string> extensions = {".tmem", ".rice.rdram", ".rice.json"};
            if (tile.at("tlut") != "None") {
                extensions.emplace_back(".rice.palette.rdram");
                extensions.emplace_back(".rice.palette.json");
            }
            bool complete = fs::file_size(raw / (base + ".tmem")) == 4096;
            for (const auto& extension : extensions) {
                auto path = raw / (base + extension);
                complete = complete && fs::is_regular_file(path) && fs::file_size(path) > 0;
                if (extension.find("json") != std::string::npos)
                    (void)readJson(path);
            }
            if (!complete)
                continue;
            for (const auto& extension : extensions)
                fs::copy_file(raw / (base + extension), ready / (base + extension),
                              fs::copy_options::overwrite_existing);
            fs::copy_file(file.path(), ready / name);
            added++;
        } catch (const std::exception&) { /* Writer has not published a complete texture yet. */
        }
    }
    return added;
}
struct Image {
    fs::path path, alpha;
    bool alternate = false, rgb = false;
    // Complete RGB/mask pairs take precedence over combined files. A combined
    // image retains transparency when a redundant, unpaired _rgb also exists.
    int priority() const {
        return !alpha.empty() ? 0 : rgb ? 2 : 1;
    }
};
struct PreparedImage {
    int width = 0, height = 0, priority = 1;
    std::vector<uint8_t> pixels, bytes;
};
std::vector<uint8_t> decode(const std::vector<uint8_t>& bytes, int& width, int& height) {
    int channels;
    if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels) ||
        width <= 0 || height <= 0 || width > 8192 || height > 8192)
        throw std::runtime_error("invalid texture PNG or dimensions");
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> decoded(
        stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 4),
        stbi_image_free);
    if (!decoded)
        throw std::runtime_error("cannot decode texture PNG");
    return {decoded.get(), decoded.get() + size_t(width) * size_t(height) * 4};
}
PreparedImage prepareBytes(std::vector<uint8_t> bytes, int priority) {
    PreparedImage result;
    result.priority = priority;
    result.bytes = std::move(bytes);
    result.pixels = decode(result.bytes, result.width, result.height);
    return result;
}
PreparedImage prepareImage(const Source& source, const Image& image) {
    auto result = prepareBytes(source.read(image.path), image.priority());
    if (image.rgb) {
        if (!image.alpha.empty()) {
            int width, height;
            auto alpha = decode(source.read(image.alpha), width, height);
            if (width != result.width || height != result.height)
                throw std::runtime_error("RGB/alpha dimensions differ: " + image.path.string());
            // Rice's _a mask uses its red component, including when the mask
            // itself is RGB/RGBA. Its own alpha channel is not the mask.
            for (size_t pixel = 0; pixel < result.pixels.size(); pixel += 4)
                result.pixels[pixel + 3] = alpha[pixel];
        } else {
            for (size_t pixel = 3; pixel < result.pixels.size(); pixel += 4)
                result.pixels[pixel] = 255;
        }
        result.bytes.clear();
        auto write = [](void *context, void *data, int length) {
            auto& output = *static_cast<std::vector<uint8_t> *>(context);
            auto begin = static_cast<const uint8_t *>(data);
            output.insert(output.end(), begin, begin + length);
        };
        if (!stbi_write_png_to_func(write, &result.bytes, result.width, result.height, 4, result.pixels.data(),
                                    result.width * 4))
            throw std::runtime_error("cannot encode combined texture PNG");
    }
    return result;
}
using ImageIndex = std::map<std::string, std::vector<Image>>;
ImageIndex indexImages(const Source& source, const std::string& romTitle) {
    static const std::regex name(
        "([^#]+)#([0-9a-f]{8}|\\$)#([0-4])#([0-3])(?:#([0-9a-f]{8}|\\$))?_(all|ciByRGBA|allciByRGBA|rgb|a)\\.png",
        std::regex::icase);
    static const std::regex alternate("^(?:(?:copy of|flash)\\s+)+", std::regex::icase);
    struct Components {
        std::string key;
        fs::path combined, rgb, alpha;
        bool alternate = false;
        int combinedPriority = 3;
    };
    std::map<fs::path, Components> components;
    ImageIndex index;
    for (const auto& file : source.paths()) {
        std::smatch match;
        std::string filename = file.filename().string();
        if (!std::regex_match(filename, match, name))
            continue;
        std::string prefix = match[1].str();
        bool isAlternate = std::regex_search(prefix, alternate);
        prefix = std::regex_replace(prefix, alternate, "");
        if (lower(prefix) != lower(romTitle))
            continue;
        std::string key = lower(match[2].str()) + "#" + match[3].str() + "#" + match[4].str();
        if (match[5].matched)
            key += "#" + lower(match[5].str());
        auto stem = lower(filename.substr(0, filename.find_last_of('_')));
        auto& parts = components[file.parent_path() / stem];
        parts.key = key;
        parts.alternate = isAlternate;
        auto kind = lower(match[6].str());
        if (kind == "rgb")
            parts.rgb = file;
        else if (kind == "a")
            parts.alpha = file;
        else {
            int priority = kind == "all" ? 0 : kind == "cibyrgba" ? 1 : 2;
            if (priority < parts.combinedPriority) {
                parts.combined = file;
                parts.combinedPriority = priority;
            }
        }
    }
    for (const auto& [base, parts] : components) {
        (void)base;
        if (!parts.rgb.empty() && !parts.alpha.empty())
            index[parts.key].push_back({parts.rgb, parts.alpha, parts.alternate, true});
        else if (!parts.combined.empty())
            index[parts.key].push_back({parts.combined, {}, parts.alternate, false});
        else if (!parts.rgb.empty())
            index[parts.key].push_back({parts.rgb, {}, parts.alternate, true});
    }
    return index;
}
std::string matchKey(const ImageIndex& images, const json& receipts, const std::string& rice) {
    auto contains = [&](const std::string& key) { return images.count(key) || receipts.contains(key); };
    if (contains(rice))
        return rice;
    static const std::regex ci("([0-9a-f]{8})#2#([0-3])#([0-9a-f]{8})");
    std::smatch fields;
    if (std::regex_match(rice, fields, ci)) {
        auto paletteWildcard = fields[1].str() + "#2#" + fields[2].str() + "#$";
        if (contains(paletteWildcard))
            return paletteWildcard;
        auto textureWildcard = "$#2#" + fields[2].str() + "#" + fields[3].str();
        if (contains(textureWildcard))
            return textureWildcard;
    }
    return {};
}
PreparedImage chooseImage(const Source& source, std::vector<Image> images) {
    bool ordinary = std::any_of(images.begin(), images.end(), [](const Image& image) { return !image.alternate; });
    if (ordinary)
        images.erase(std::remove_if(images.begin(), images.end(), [](const Image& image) { return image.alternate; }),
                     images.end());
    int priority = std::min_element(images.begin(), images.end(), [](const Image& a, const Image& b) {
                       return a.priority() < b.priority();
                   })->priority();
    images.erase(std::remove_if(images.begin(), images.end(),
                                [priority](const Image& image) { return image.priority() != priority; }),
                 images.end());
    std::sort(images.begin(), images.end(), [](const Image& a, const Image& b) { return a.path < b.path; });
    auto selected = prepareImage(source, images.front());
    for (size_t i = 1; i < images.size(); i++) {
        auto other = prepareImage(source, images[i]);
        if (other.width != selected.width || other.height != selected.height || other.pixels != selected.pixels)
            throw std::runtime_error("conflicting Rice images: " + images.front().path.string());
    }
    return selected;
}
}

Scanner::Scanner(ScanConfig value) : config(std::move(value)) {
    config.source = fs::canonical(config.source);
    config.raw = fs::absolute(config.raw).lexically_normal();
    config.replacements = fs::absolute(config.replacements).lexically_normal();
    if (!fs::is_directory(config.source) && !fs::is_regular_file(config.source))
        throw std::runtime_error("--texture-scan requires a ZIP file or extracted directory");
    auto nested = [](const fs::path& a, const fs::path& b) {
        auto relative = a.lexically_relative(b);
        return !relative.empty() && *relative.begin() != "..";
    };
    if (nested(config.source, config.replacements) || nested(config.replacements, config.source) ||
        nested(config.source, config.raw) || nested(config.raw, config.source))
        throw std::runtime_error("texture scan source must differ from output directories");
    if (fs::exists(config.replacements) && !fs::is_directory(config.replacements))
        throw std::runtime_error("texture scan output must be a directory");
    if (fs::exists(config.replacements / "rt64.json") &&
        !readJson(config.replacements / "rt64.json").value("textureScan", false))
        throw std::runtime_error("texture scan cannot overwrite an external indexed RT64 pack");
    fs::create_directories(config.raw);
    fs::create_directories(config.replacements);
}
Scanner::~Scanner() {
    stop();
}
void Scanner::start() {
    worker = std::thread(&Scanner::run, this);
}
void Scanner::stop() {
    {
        std::lock_guard lock(mutex);
        stopping = true;
    }
    changed.notify_all();
    if (worker.joinable())
        worker.join();
}
void Scanner::run() {
    // Hashing, matching, and packing all run on this lower-priority thread.
    setpriority(PRIO_PROCESS, 0, 10);
    std::unique_lock lock(mutex);
    while (!stopping) {
        lock.unlock();
        try {
            scanOnce();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "Texture scan: %s\n", error.what());
        }
        lock.lock();
        changed.wait_for(lock, std::chrono::seconds(ScanIntervalSeconds), [this] { return stopping; });
    }
}
size_t Scanner::scanOnce() {
    // Prevent two game instances from publishing the same pack concurrently.
    auto lockPath = config.replacements / ".texture-scan.lock";
    int descriptor = open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (descriptor < 0)
        throw std::runtime_error("cannot open texture scan lock");
    struct Close {
        int descriptor;
        ~Close() {
            close(descriptor);
        }
    } closeLock{descriptor};
    if (flock(descriptor, LOCK_EX | LOCK_NB))
        return 0;
    auto ready = config.raw / "ready";
    fs::create_directories(ready);
    size_t added = snapshot(config.raw, ready);
    auto databasePath = ready / "rt64.json";
    auto pending = ready / ".hash-pending";
    if (added)
        writeJson(pending, {{"pending", true}});
    if (fs::exists(pending) || !fs::exists(databasePath)) {
        RT64::generateRiceHashes(ready);
        fs::remove(pending);
    }
    auto database = readJson(databasePath);
    Source source(config.source);
    auto images = indexImages(source, config.romTitle);
    Pack pack(config.replacements);
    auto receiptPath = config.replacements / ".texture-scan.json";
    json receipts = json::object();
    json formats = json::object();
    if (fs::is_regular_file(receiptPath)) {
        auto stored = readJson(receiptPath);
        if (stored.value("source", "") == config.source.string()) {
            receipts = stored.at("textures");
            formats = stored.value("formats", json::object());
        }
    }
    static const std::regex filename("[0-9a-f]{16}\\.v5\\.png");
    static const std::regex hash("[0-9a-f]{16}");
    for (auto it = receipts.begin(); it != receipts.end();) {
        if (!it.value().is_string() || !std::regex_match(it.value().get<std::string>(), filename) ||
            !pack.contains(it.value().get<std::string>()))
            it = receipts.erase(it);
        else
            ++it;
    }
    if (images.empty() && receipts.empty())
        throw std::runtime_error("no Rice PNG filenames for ROM \"" + config.romTitle + "\" in " +
                                 config.source.string());
    size_t imported = 0;
    bool published = false;
    struct Notify {
        std::atomic<bool>& pending;
        bool& published;
        ~Notify() {
            if (published)
                pending = true;
        }
    } notify{reloadPending, published};
    // Group aliases so only one decoded image lives in memory at a time.
    std::map<std::string, std::set<std::string>> groups;
    for (const auto& entry : database.at("textures")) {
        auto rt64 = entry.at("hashes").at("rt64").get<std::string>();
        if (!std::regex_match(rt64, hash))
            continue;
        auto rice = lower(entry.at("hashes").at("rice").get<std::string>());
        auto key = matchKey(images, receipts, rice);
        if (!key.empty())
            groups[key].insert(rt64 + ".v5.png");
    }
    for (const auto& [key, outputs] : groups) {
        auto found = images.find(key);
        std::string inputs;
        if (found != images.end()) {
            std::vector<std::string> stamps;
            for (const auto& candidate : found->second) {
                stamps.push_back(source.stamp(candidate.path));
                if (!candidate.alpha.empty())
                    stamps.push_back(source.stamp(candidate.alpha));
            }
            std::sort(stamps.begin(), stamps.end());
            for (const auto& value : stamps)
                inputs += value + "\n";
        } else {
            inputs = pack.stamp(receipts.at(key).get<std::string>());
        }
        bool unchanged = inputStamps.count(key) && inputStamps.at(key) == inputs;
        for (const auto& name : outputs) {
            auto current = pack.stamp(name);
            unchanged = unchanged && !current.empty() && outputStamps.count(name) && outputStamps.at(name) == current;
        }
        if (unchanged)
            continue;
        bool ordinary = found != images.end() && std::any_of(found->second.begin(), found->second.end(),
                                                             [](const Image& image) { return !image.alternate; });
        PreparedImage image;
        bool cached = true;
        if (found != images.end() && (ordinary || !receipts.contains(key))) {
            image = chooseImage(source, found->second);
            // Old receipts came only from complete _all/_ciByRGBA images.
            cached = receipts.contains(key) && image.priority > formats.value(key, 1);
        }
        if (cached) {
            image = prepareBytes(pack.read(receipts.at(key).get<std::string>()), formats.value(key, 1));
        }
        for (const auto& name : outputs) {
            if (!pack.contains(name) || pack.read(name) != image.bytes) {
                pack.put(name, image.bytes);
                imported++;
            }
            receipts[key] = name;
            outputStamps[name] = pack.stamp(name);
        }
        formats[key] = image.priority;
        inputStamps[key] = inputs;
    }
    published = pack.finish();
    // Retain reusable aliases, including mappings from earlier move-based imports.
    writeJson(receiptPath, {{"source", config.source.string()}, {"textures", receipts}, {"formats", formats}});
    pack.cleanLooseFiles();
    if (published)
        std::fprintf(stderr,
                     "Texture scan: imported %zu textures; published textures.rtz and rt64.json; refresh pending.\n",
                     imported);
    return imported;
}
}
