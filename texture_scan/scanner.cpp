#include "scanner.h"

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
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
#include "json/json.hpp"
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"

extern char **environ;
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
std::vector<uint8_t> readBytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream)
        throw std::runtime_error("cannot read " + path.string());
    auto length = stream.tellg();
    if (length <= 0 || length > 256 * 1024 * 1024)
        throw std::runtime_error("invalid image size: " + path.string());
    std::vector<uint8_t> bytes(static_cast<size_t>(length));
    stream.seekg(0);
    stream.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
    if (!stream)
        throw std::runtime_error("incomplete image: " + path.string());
    return bytes;
}
// .tile.json is written last by RT64. A successfully parsed tile plus all
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
void hashCaptures(const fs::path& hasher, const fs::path& ready, const fs::path& log) {
    std::string executable = hasher.string(), directory = ready.string();
    char *arguments[] = {executable.data(), directory.data(), const_cast<char *>("--rice"), nullptr};
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions))
        throw std::runtime_error("cannot initialize texture hasher");
    int action =
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (!action)
        action = posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    pid_t pid = -1;
    int error = action ? action : posix_spawn(&pid, executable.c_str(), &actions, nullptr, arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (error)
        throw std::runtime_error("cannot start texture_hasher: " + std::string(std::strerror(error)));
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            throw std::runtime_error("cannot wait for texture_hasher");
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status))
        throw std::runtime_error("texture_hasher failed; see " + log.string());
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
    std::vector<fs::path> sources;
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
PreparedImage prepareImage(const Image& image) {
    PreparedImage result;
    result.priority = image.priority();
    result.bytes = readBytes(image.path);
    result.pixels = decode(result.bytes, result.width, result.height);
    result.sources.push_back(image.path);
    if (image.rgb) {
        if (!image.alpha.empty()) {
            int width, height;
            auto alpha = decode(readBytes(image.alpha), width, height);
            if (width != result.width || height != result.height)
                throw std::runtime_error("RGB/alpha dimensions differ: " + image.path.string());
            // Rice's _a mask uses its red component, including when the mask
            // itself is RGB/RGBA. Its own alpha channel is not the mask.
            for (size_t pixel = 0; pixel < result.pixels.size(); pixel += 4)
                result.pixels[pixel + 3] = alpha[pixel];
            result.sources.push_back(image.alpha);
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
ImageIndex indexImages(const fs::path& source, const std::string& romTitle) {
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
    for (const auto& file : fs::recursive_directory_iterator(source)) {
        if (!file.is_regular_file() || file.is_symlink())
            continue;
        std::smatch match;
        std::string filename = file.path().filename().string();
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
        auto& parts = components[file.path().parent_path() / stem];
        parts.key = key;
        parts.alternate = isAlternate;
        auto kind = lower(match[6].str());
        if (kind == "rgb")
            parts.rgb = file.path();
        else if (kind == "a")
            parts.alpha = file.path();
        else {
            int priority = kind == "all" ? 0 : kind == "cibyrgba" ? 1 : 2;
            if (priority < parts.combinedPriority) {
                parts.combined = file.path();
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
PreparedImage chooseImage(std::vector<Image> images) {
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
    auto selected = prepareImage(images.front());
    for (size_t i = 1; i < images.size(); i++) {
        auto other = prepareImage(images[i]);
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
    if (!fs::is_directory(config.source))
        throw std::runtime_error("--texture-scan requires an extracted texture directory");
    auto nested = [](const fs::path& a, const fs::path& b) {
        auto relative = a.lexically_relative(b);
        return !relative.empty() && *relative.begin() != "..";
    };
    if (nested(config.source, config.replacements) || nested(config.replacements, config.source) ||
        nested(config.source, config.raw) || nested(config.raw, config.source))
        throw std::runtime_error("texture scan source must differ from output directories");
    if (fs::exists(config.replacements / "rt64.json"))
        throw std::runtime_error("texture scan output must be a hash-named folder, not an indexed RT64 pack");
    if (!fs::is_regular_file(config.hasher))
        throw std::runtime_error("texture_hasher executable is missing: " + config.hasher.string());
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
    std::unique_lock lock(mutex);
    while (!stopping) {
        lock.unlock();
        try {
            scanOnce();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "Texture scan: %s\n", error.what());
        }
        lock.lock();
        changed.wait_for(lock, std::chrono::seconds(15), [this] { return stopping; });
    }
}
size_t Scanner::scanOnce() {
    // Prevent two game instances from moving the same images concurrently.
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
    size_t added;
    {
        std::lock_guard lock(captures);
        added = snapshot(config.raw, ready);
    }
    auto databasePath = ready / "rt64.json";
    auto pending = ready / ".hash-pending";
    if (added)
        writeJson(pending, {{"pending", true}});
    if (fs::exists(pending) || !fs::exists(databasePath)) {
        hashCaptures(config.hasher, ready, config.raw / "hasher.log");
        fs::remove(pending);
    }
    auto database = readJson(databasePath);
    auto images = indexImages(config.source, config.romTitle);
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
            !fs::is_regular_file(config.replacements / it.value().get<std::string>()))
            it = receipts.erase(it);
        else
            ++it;
    }
    if (images.empty() && receipts.empty())
        throw std::runtime_error("no Rice PNG filenames for ROM \"" + config.romTitle + "\" in " +
                                 config.source.string());
    size_t imported = 0;
    struct Notify {
        std::atomic<bool>& pending;
        size_t& count;
        ~Notify() {
            if (count)
                pending = true;
        }
    } notify{reloadPending, imported};
    std::set<fs::path> consumed;
    std::map<std::string, PreparedImage> prepared;
    for (const auto& entry : database.at("textures")) {
        auto rt64 = entry.at("hashes").at("rt64").get<std::string>();
        if (!std::regex_match(rt64, hash))
            continue;
        auto rice = lower(entry.at("hashes").at("rice").get<std::string>());
        auto key = matchKey(images, receipts, rice);
        if (key.empty())
            continue;
        if (!prepared.count(key)) {
            auto found = images.find(key);
            bool ordinary = found != images.end() && std::any_of(found->second.begin(), found->second.end(),
                                                                 [](const Image& image) { return !image.alternate; });
            PreparedImage image;
            bool cached = true;
            if (found != images.end() && (ordinary || !receipts.contains(key))) {
                image = chooseImage(found->second);
                // Old receipts came only from complete _all/_ciByRGBA images.
                // Do not replace their transparency with a leftover _rgb file.
                cached = receipts.contains(key) && image.priority > formats.value(key, 1);
            }
            if (cached) {
                image = prepareImage({config.replacements / receipts.at(key).get<std::string>(), {}, false, false});
                image.priority = formats.value(key, 1);
                image.sources.clear();
            }
            prepared.emplace(key, std::move(image));
        }
        const auto& image = prepared.at(key);
        const auto& bytes = image.bytes;
        std::string outputName = rt64 + ".v5.png";
        auto output = config.replacements / outputName;
        if (!fs::is_regular_file(output) || readBytes(output) != bytes) {
            auto temporary = output;
            temporary += ".scan-tmp";
            std::ofstream stream(temporary, std::ios::binary);
            stream.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            stream.close();
            if (!stream)
                throw std::runtime_error("cannot write " + temporary.string());
            fs::rename(temporary, output);
            imported++;
        }
        receipts[key] = outputName;
        formats[key] = image.priority;
        consumed.insert(image.sources.begin(), image.sources.end());
    }
    // Record reusable aliases before consuming originals (including CI '*'
    // matches which can acquire new palette-specific RT64 hashes later).
    writeJson(receiptPath, {{"source", config.source.string()}, {"textures", receipts}, {"formats", formats}});
    for (const auto& source : consumed)
        fs::remove(source);
    if (imported)
        std::fprintf(stderr, "Texture scan: imported %zu textures; refresh pending.\n", imported);
    return imported;
}
}
