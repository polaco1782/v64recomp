#include "texture_editor_zip.h"
#include "common/rt64_filesystem_zip.h"
#include "common/rt64_mapped_file.h"
#include "miniz/miniz.h"
#include "stb/stb_image.h"
#include "stb/stb_image_write.h"
#include <array>
#include <cstring>
#include <fstream>
#include <random>
#include <stdexcept>

namespace fs = std::filesystem;
namespace RecompTextureZip {
namespace {
constexpr size_t maxPngSize = 256 * 1024 * 1024;
struct ZipReader {
    RT64::MappedFile file;
    mz_zip_archive archive{};
    explicit ZipReader(const fs::path& path) : file(path) {
        if (!file.isOpen() ||
            !mz_zip_reader_init_mem(&archive, file.data(), file.size(), MZ_ZIP_FLAG_DO_NOT_SORT_CENTRAL_DIRECTORY))
            throw std::runtime_error("Could not read texture ZIP.");
    }
    ~ZipReader() {
        mz_zip_reader_end(&archive);
    }
};

FILE *openOutput(const fs::path& path) {
#ifdef _WIN32
    return _wfopen(path.c_str(), L"wb+");
#else
    return std::fopen(path.c_str(), "wb+");
#endif
}

struct ZipWriter {
    std::unique_ptr<FILE, decltype(&std::fclose)> file;
    mz_zip_archive archive{};
    explicit ZipWriter(const fs::path& path) : file(openOutput(path), std::fclose) {
        if (!file || !mz_zip_writer_init_cfile(&archive, file.get(), MZ_ZIP_FLAG_WRITE_ZIP64))
            throw std::runtime_error("Could not create temporary texture ZIP.");
    }
    ~ZipWriter() {
        if (archive.m_pState)
            mz_zip_writer_end(&archive);
    }
    void finish() {
        if (!mz_zip_writer_finalize_archive(&archive) || !mz_zip_writer_end(&archive) || std::fclose(file.release()))
            throw std::runtime_error("Could not finish writing texture ZIP.");
    }
};

void writeArchive(const fs::path& original, const fs::path& output, const std::string& entry,
                  const std::vector<uint8_t>& png) {
    ZipReader reader(original);
    if (reader.file.size() < 4 || std::memcmp(reader.file.data(), "PK\x03\x04", 4) != 0)
        throw std::runtime_error("Editing requires a standalone texture ZIP.");
    ZipWriter writer(output);
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&reader.archive); i++) {
        const auto count = mz_zip_reader_get_filename(&reader.archive, i, nullptr, 0);
        std::string name(count, '\0');
        if (!count || !mz_zip_reader_get_filename(&reader.archive, i, name.data(), count))
            throw std::runtime_error("Could not read ZIP entry name.");
        name.pop_back();
        if (RT64::FileSystem::toForwardSlashes(name) == entry)
            continue;
        if (!mz_zip_writer_add_from_zip_reader(&writer.archive, &reader.archive, i))
            throw std::runtime_error("Could not copy ZIP entry: " + name);
    }
    if (!mz_zip_writer_add_mem(&writer.archive, entry.c_str(), png.data(), png.size(), 0))
        throw std::runtime_error("Could not write the new texture.");
    writer.finish();
}

void appendPng(void *context, void *data, int size) {
    auto& bytes = *static_cast<std::vector<uint8_t> *>(context);
    auto *begin = static_cast<uint8_t *>(data);
    bytes.insert(bytes.end(), begin, begin + size);
}
}

std::vector<uint8_t> readPng(const fs::path& path) {
    const auto size = fs::file_size(path);
    if (!size || size > maxPngSize)
        throw std::runtime_error("Texture PNG exceeds the supported file size.");
    std::ifstream file(path, std::ios::binary);
    std::vector<uint8_t> bytes(size);
    if (!file.read(reinterpret_cast<char *>(bytes.data()), bytes.size()))
        throw std::runtime_error("Could not read texture PNG.");
    return bytes;
}

Image decodePng(const std::vector<uint8_t>& bytes) {
    constexpr std::array<uint8_t, 8> signature{137, 80, 78, 71, 13, 10, 26, 10};
    Image result;
    int channels = 0;
    if (bytes.size() < signature.size() || bytes.size() > maxPngSize ||
        !std::equal(signature.begin(), signature.end(), bytes.begin()) ||
        !stbi_info_from_memory(bytes.data(), int(bytes.size()), &result.width, &result.height, &channels) ||
        result.width < 1 || result.height < 1 || result.width > 8192 || result.height > 8192)
        throw std::runtime_error("Choose a PNG with dimensions up to 8192 x 8192.");
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
        stbi_load_from_memory(bytes.data(), int(bytes.size()), &result.width, &result.height, &channels, 4),
        stbi_image_free);
    if (!pixels)
        throw std::runtime_error("Could not decode texture PNG.");
    result.pixels.assign(pixels.get(), pixels.get() + size_t(result.width) * result.height * 4);
    return result;
}

std::vector<uint8_t> encodePng(const Image& image) {
    std::vector<uint8_t> bytes;
    if (!stbi_write_png_to_func(appendPng, &bytes, image.width, image.height, 4, image.pixels.data(), image.width * 4))
        throw std::runtime_error("Could not encode replacement PNG.");
    return bytes;
}

Edit::Edit(const fs::path& path, const std::string& entry, const std::vector<uint8_t>& png)
    : zip(path), size(fs::file_size(zip)), modified(fs::last_write_time(zip)) {
    std::random_device random;
    do {
        directory = zip.parent_path() / (".v64recomp-texture-" + std::to_string(random()));
    } while (!fs::create_directory(directory));

    try {
        const auto staged = directory / "pack.zip";
        writeArchive(zip, staged, entry, png);
        fs::permissions(staged, fs::status(zip).permissions());
        auto result = RT64::FileSystemZip::create(staged, {});
        std::vector<uint8_t> saved;
        if (!result || !result->load(entry, saved) || saved != png)
            throw std::runtime_error("Could not verify the updated texture ZIP.");
    } catch (...) {
        std::error_code error;
        fs::remove_all(directory, error);
        throw;
    }
}

Edit::~Edit() {
    std::error_code error;
    fs::remove_all(directory, error);
}

void Edit::publish() {
    if (fs::file_size(zip) != size || fs::last_write_time(zip) != modified)
        throw std::runtime_error("The texture ZIP changed while the edit was being saved. Reopen the editor.");
#ifdef _WIN32
    if (!MoveFileExW((directory / "pack.zip").c_str(), zip.c_str(), MOVEFILE_REPLACE_EXISTING))
        throw std::system_error(GetLastError(), std::system_category(), "Could not replace texture ZIP");
#else
    fs::rename(directory / "pack.zip", zip);
#endif
}
}
