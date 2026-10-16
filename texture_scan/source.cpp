#include "source.h"
#include "miniz/miniz.h"
#include <algorithm>
#include <fstream>
#include <map>
#include <stdexcept>

namespace fs = std::filesystem;
namespace RecompTextures {
struct Source::Impl {
    struct Entry {
        mz_uint index;
        uint64_t size;
        uint32_t crc;
    };
    fs::path path;
    mz_zip_archive zip{};
    std::map<fs::path, Entry> entries;
    explicit Impl(fs::path path) : path(std::move(path)) {}
    ~Impl() {
        if (zip.m_pState)
            mz_zip_reader_end(&zip);
    }
};
Source::Source(const fs::path& path) : impl(std::make_unique<Impl>(path)) {
    if (fs::is_directory(path)) {
        for (const auto& file : fs::recursive_directory_iterator(path)) {
            if (file.is_regular_file() && !file.is_symlink())
                impl->entries.emplace(file.path().lexically_relative(path), Impl::Entry{});
        }
    } else {
        if (!mz_zip_reader_init_file(&impl->zip, path.c_str(), 0))
            throw std::runtime_error("cannot open texture source ZIP: " + path.string());
        for (mz_uint index = 0; index < mz_zip_reader_get_num_files(&impl->zip); index++) {
            mz_zip_archive_file_stat info{};
            if (!mz_zip_reader_file_stat(&impl->zip, index, &info))
                throw std::runtime_error("cannot index texture source ZIP");
            if (info.m_is_directory)
                continue;
            auto length = mz_zip_reader_get_filename(&impl->zip, index, nullptr, 0);
            std::string name(length, '\0');
            if (!length || !mz_zip_reader_get_filename(&impl->zip, index, name.data(), name.size()))
                throw std::runtime_error("cannot read texture ZIP entry name");
            name.resize(length - 1);
            std::replace(name.begin(), name.end(), '\\', '/');
            if (!impl->entries.emplace(fs::u8path(name), Impl::Entry{index, info.m_uncomp_size, info.m_crc32}).second)
                throw std::runtime_error("duplicate path in texture source ZIP: " + name);
        }
    }
}
Source::~Source() = default;
std::vector<fs::path> Source::paths() const {
    std::vector<fs::path> result;
    result.reserve(impl->entries.size());
    for (const auto& [path, entry] : impl->entries) {
        (void)entry;
        result.push_back(path);
    }
    return result;
}
std::string Source::stamp(const fs::path& path) const {
    if (impl->zip.m_pState) {
        const auto& entry = impl->entries.at(path);
        return path.generic_string() + ":" + std::to_string(entry.crc) + ":" + std::to_string(entry.size);
    }
    const auto file = impl->path / path;
    return path.generic_string() + ":" + std::to_string(fs::file_size(file)) + ":" +
           std::to_string(fs::last_write_time(file).time_since_epoch().count());
}
std::vector<uint8_t> Source::read(const fs::path& path) const {
    const auto& entry = impl->entries.at(path);
    const auto file = impl->path / path;
    auto size = impl->zip.m_pState ? entry.size : fs::file_size(file);
    if (!size || size > 256 * 1024 * 1024)
        throw std::runtime_error("invalid texture source image size: " + path.string());
    std::vector<uint8_t> bytes(size);
    if (impl->zip.m_pState) {
        if (!mz_zip_reader_extract_to_mem(&impl->zip, entry.index, bytes.data(), bytes.size(), 0))
            throw std::runtime_error("cannot read texture ZIP entry: " + path.string());
    } else {
        std::ifstream stream(file, std::ios::binary);
        stream.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
        if (!stream)
            throw std::runtime_error("cannot read texture source image: " + file.string());
    }
    return bytes;
}
}
