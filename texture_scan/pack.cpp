#include "pack.h"
#include "common/rt64_replacement_database.h"
#include "miniz/miniz.h"
#include "stb/stb_image.h"
#include <fstream>
#include <map>
#include <regex>
#include <stdexcept>

namespace fs = std::filesystem;
namespace RecompTextures {
namespace {
struct Zip {
    mz_zip_archive archive{};
    bool writer;
    explicit Zip(bool writer) : writer(writer) {}
    ~Zip() {
        if (archive.m_pState) {
            if (writer)
                mz_zip_writer_end(&archive);
            else
                mz_zip_reader_end(&archive);
        }
    }
};
const std::regex textureName("[0-9a-f]{16}\\.v5\\.png");
constexpr size_t maxImageBytes = 256 * 1024 * 1024;
}
struct Pack::Impl {
    struct Entry {
        int index = -1;
        uint32_t crc = 0;
        uint64_t size = 0;
        fs::path loose;
        bool updated = false;
    };
    fs::path directory, path, temporary;
    Zip reader{false}, writer{true};
    std::map<std::string, Entry> entries;
    explicit Impl(const fs::path& directory)
        : directory(directory), path(directory / "textures.rtz"), temporary(directory / "textures.rtz.scan-tmp") {
        if (fs::exists(path)) {
            if (!mz_zip_reader_init_file(&reader.archive, path.c_str(), 0))
                throw std::runtime_error("cannot read texture archive " + path.string());
            for (mz_uint index = 0; index < mz_zip_reader_get_num_files(&reader.archive); index++) {
                mz_zip_archive_file_stat info{};
                if (!mz_zip_reader_file_stat(&reader.archive, index, &info))
                    throw std::runtime_error("cannot index texture archive");
                std::string name = info.m_filename;
                if (std::regex_match(name, textureName))
                    entries[name] = {static_cast<int>(index), info.m_crc32, info.m_uncomp_size, {}, false};
            }
            int index = mz_zip_reader_locate_file(&reader.archive, "rt64.json", nullptr, 0);
            if (index < 0)
                throw std::runtime_error("texture archive has no RT64 index");
            auto bytes = extract(index);
            auto metadata = json::parse(bytes.begin(), bytes.end());
            if (!metadata.value("textureScan", false))
                throw std::runtime_error("texture archive is not owned by the scanner");
        }
        for (const auto& file : fs::directory_iterator(directory)) {
            auto name = file.path().filename().string();
            if (file.is_regular_file() && std::regex_match(name, textureName))
                entries[name].loose = file.path();
        }
    }
    ~Impl() {
        // Close before removing the abandoned transaction, including on errors.
        if (writer.archive.m_pState)
            mz_zip_writer_end(&writer.archive);
        std::error_code ignored;
        fs::remove(temporary, ignored);
    }
    std::vector<uint8_t> extract(int index) const {
        auto archive = const_cast<mz_zip_archive *>(&reader.archive);
        mz_zip_archive_file_stat info{};
        if (!mz_zip_reader_file_stat(archive, index, &info) || !info.m_uncomp_size ||
            info.m_uncomp_size > maxImageBytes)
            throw std::runtime_error("invalid texture archive entry size");
        std::vector<uint8_t> bytes(info.m_uncomp_size);
        if (!mz_zip_reader_extract_to_mem(archive, index, bytes.data(), bytes.size(), 0))
            throw std::runtime_error("cannot extract texture archive entry");
        return bytes;
    }
    void start() {
        if (!writer.archive.m_pState &&
            !mz_zip_writer_init_file_v2(&writer.archive, temporary.c_str(), 0, MZ_ZIP_FLAG_WRITE_ZIP64))
            throw std::runtime_error("cannot create texture archive " + temporary.string());
    }
    void writeIndex(const std::string& bytes) {
        auto index = directory / "rt64.json.tmp";
        std::ofstream stream(index, std::ios::binary);
        stream.write(bytes.data(), bytes.size());
        stream.close();
        if (!stream)
            throw std::runtime_error("cannot write RT64 texture index");
        fs::rename(index, directory / "rt64.json");
    }
};
Pack::Pack(const fs::path& directory) : impl(std::make_unique<Impl>(directory)) {}
Pack::~Pack() = default;
bool Pack::contains(const std::string& name) const {
    return impl->entries.count(name);
}
std::string Pack::stamp(const std::string& name) const {
    auto it = impl->entries.find(name);
    if (it == impl->entries.end())
        return {};
    const auto& entry = it->second;
    if (!entry.loose.empty() && !entry.updated)
        return entry.loose.string() + ":" + std::to_string(fs::file_size(entry.loose)) + ":" +
               std::to_string(fs::last_write_time(entry.loose).time_since_epoch().count());
    return std::to_string(entry.crc) + ":" + std::to_string(entry.size);
}
std::vector<uint8_t> Pack::read(const std::string& name) const {
    const auto& entry = impl->entries.at(name);
    if (!entry.loose.empty()) {
        auto size = fs::file_size(entry.loose);
        if (!size || size > maxImageBytes)
            throw std::runtime_error("invalid loose texture size");
        std::vector<uint8_t> bytes(size);
        std::ifstream stream(entry.loose, std::ios::binary);
        stream.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
        if (!stream)
            throw std::runtime_error("cannot read " + entry.loose.string());
        return bytes;
    }
    return impl->extract(entry.index);
}
void Pack::put(const std::string& name, const std::vector<uint8_t>& bytes) {
    if (!std::regex_match(name, textureName) || bytes.empty() || bytes.size() > maxImageBytes)
        throw std::runtime_error("invalid packed texture");
    int width, height, channels;
    if (!stbi_info_from_memory(bytes.data(), bytes.size(), &width, &height, &channels) || width <= 0 || height <= 0 ||
        width > 8192 || height > 8192)
        throw std::runtime_error("invalid replacement PNG: " + name);
    impl->start();
    auto& entry = impl->entries[name];
    if (entry.updated)
        throw std::runtime_error("duplicate texture in pack transaction");
    // PNG data is already compressed. Store it directly to keep packing work
    // bounded and avoid competing with the game for compression threads.
    if (!mz_zip_writer_add_mem(&impl->writer.archive, name.c_str(), bytes.data(), bytes.size(), MZ_NO_COMPRESSION))
        throw std::runtime_error("cannot add texture to archive");
    entry.crc = mz_crc32(MZ_CRC32_INIT, bytes.data(), bytes.size());
    entry.size = bytes.size();
    entry.updated = true;
}
bool Pack::finish() {
    for (const auto& [name, entry] : impl->entries) {
        if (!entry.loose.empty() && !entry.updated)
            put(name, read(name));
    }
    if (!impl->writer.archive.m_pState) {
        if (impl->reader.archive.m_pState && !fs::exists(impl->directory / "rt64.json")) {
            auto bytes = impl->extract(mz_zip_reader_locate_file(&impl->reader.archive, "rt64.json", nullptr, 0));
            impl->writeIndex(std::string(bytes.begin(), bytes.end()));
        }
        return false;
    }
    RT64::ReplacementDatabase database;
    database.config.defaultOperation = RT64::ReplacementOperation::Stall;
    for (const auto& [name, entry] : impl->entries) {
        if (!entry.updated &&
            !mz_zip_writer_add_from_zip_reader(&impl->writer.archive, &impl->reader.archive, entry.index))
            throw std::runtime_error("cannot retain existing packed texture");
        RT64::ReplacementTexture texture;
        texture.path = name;
        texture.hashes.rt64 = name.substr(0, 16);
        database.addReplacement(texture);
    }
    json metadata = database;
    metadata["textureScan"] = true;
    std::string bytes = metadata.dump(2) + "\n";
    if (!mz_zip_writer_add_mem(&impl->writer.archive, "rt64.json", bytes.data(), bytes.size(), MZ_BEST_SPEED) ||
        !mz_zip_writer_finalize_archive(&impl->writer.archive))
        throw std::runtime_error("cannot finalize texture archive");
    if (!mz_zip_writer_end(&impl->writer.archive))
        throw std::runtime_error("cannot close texture archive");
    // RT64 reads the self-contained archive. Replacing its inode keeps the old
    // mapped pack valid until the host reloads the completed new generation.
    impl->writeIndex(bytes);
    fs::rename(impl->temporary, impl->path);
    return true;
}
void Pack::cleanLooseFiles() {
    for (const auto& [name, entry] : impl->entries) {
        (void)name;
        if (!entry.loose.empty())
            fs::remove(entry.loose);
    }
}
}
