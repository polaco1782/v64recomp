#include "editor_ai.h"
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <fcntl.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace fs = std::filesystem;

struct TemporaryDirectory {
    fs::path path;
    bool create(const fs::path& parent) {
#ifdef _WIN32
        std::random_device random;
        auto candidate = parent / ("v64-texture-" + std::to_string(random()) + "-" + std::to_string(random()));
        if (!fs::create_directory(candidate))
            return false;
        path = std::move(candidate);
#else
        auto pattern = (parent / "v64-texture-XXXXXX").string();
        if (!mkdtemp(pattern.data()))
            return false;
        path = std::move(pattern);
#endif
        return true;
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        if (!path.empty())
            fs::remove_all(path, ignored);
    }
};

struct Gles2AiJob {
    TemporaryDirectory work;
    unsigned width, height;
#ifdef _WIN32
    HANDLE group = nullptr, process = nullptr;
#else
    pid_t pid = -1;
#endif
};

#ifdef _WIN32
// Quote one Windows argv element, including backslashes preceding a quote or the closing delimiter.
static std::wstring quote(const std::string& utf8) {
    std::wstring arg = fs::u8path(utf8).wstring(), result = L"\"";
    size_t slashes = 0;
    for (wchar_t c : arg) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += c;
        slashes = 0;
    }
    return result.append(slashes * 2, L'\\') + L"\"";
}
#endif

static bool launch(Gles2AiJob& job, const std::string& prompt) {
    const std::string dir = job.work.path.u8string(), source = (job.work.path / "source.png").u8string();
    const char *args[] = {"codex",
                          "exec",
                          "--ephemeral",
                          "--sandbox",
                          "workspace-write",
                          "--skip-git-repo-check",
                          "-C",
                          dir.c_str(),
                          "--image",
                          source.c_str(),
                          "--",
                          prompt.c_str(),
                          nullptr};
#ifdef _WIN32
    std::wstring command;
    for (auto arg = args; *arg; ++arg)
        command += quote(*arg) + L" ";
    job.group = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.group || !SetInformationJobObject(job.group, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return false;
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr,
                        job.work.path.c_str(), &startup, &child))
        return false;
    job.process = child.hProcess;
    bool ok = AssignProcessToJobObject(job.group, child.hProcess) && ResumeThread(child.hThread) != (DWORD)-1;
    if (!ok)
        TerminateProcess(child.hProcess, 1);
    CloseHandle(child.hThread);
    return ok;
#else
    posix_spawnattr_t attr;
    if (posix_spawnattr_init(&attr))
        return false;
    int error = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    if (!error)
        error = posix_spawnattr_setpgroup(&attr, 0);
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions)) {
        posix_spawnattr_destroy(&attr);
        return false;
    }
    // The CLI reads piped stdin in addition to its prompt. Never attach the game's input.
    if (!error)
        error = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    pid_t child;
    if (!error)
        error = posix_spawnp(&child, args[0], &actions, &attr, const_cast<char **>(args), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (!error)
        job.pid = child;
    return !error;
#endif
}

void gles2_ai_destroy(Gles2AiJob *job) {
    if (!job)
        return;
#ifdef _WIN32
    if (job->group)
        CloseHandle(job->group);
    if (job->process) {
        WaitForSingleObject(job->process, INFINITE);
        CloseHandle(job->process);
    }
#else
    if (job->pid > 0) {
        kill(-job->pid, SIGKILL);
        while (waitpid(job->pid, nullptr, 0) < 0 && errno == EINTR) {
        }
    }
#endif
    delete job;
}

Gles2AiJob *gles2_ai_start(const uint8_t *rgba, unsigned width, unsigned height, const char **error) {
    *error = "Could not prepare the Codex workspace";
    try {
        std::unique_ptr<Gles2AiJob, decltype(&gles2_ai_destroy)> job(new Gles2AiJob{}, gles2_ai_destroy);
        job->width = width * GLES2_AI_SCALE;
        job->height = height * GLES2_AI_SCALE;
        if (!job->work.create(fs::temp_directory_path()))
            return nullptr;
        size_t size = 0;
        std::unique_ptr<void, decltype(&mz_free)> png(
            tdefl_write_image_to_png_file_in_memory(rgba, width, height, 4, &size), mz_free);
        std::ofstream file(job->work.path / "source.png", std::ios::binary);
        if (!png || !file.write(static_cast<const char *>(png.get()), size))
            return nullptr;
        file.close();
        if (!file)
            return nullptr;
        std::string prompt =
            "Use image generation to edit the attached source.png, a single N64 game texture. "
            "Recreate this exact pattern at higher resolution. Preserve every motif, its position, scale, orientation, "
            "repeat count and spacing, edge-to-edge tiling, palette, shading and alpha silhouette. Keep lettering "
            "and symbols legible and faithful. For markings stored upside down or mirrored, inspect a flipped "
            "view to identify the shapes and deliver the edit in the original stored orientation. Reconstruct "
            "smooth contours and consistent strokes for printed markings. Use the native tile as a layout "
            "reference. Remove pixelation artifacts: blocky texel edges, stair-stepped diagonals, color banding "
            "and low-resolution dithering. Keep edges crisp and gradients smooth. Avoid sharpening halos and "
            "per-texel color matching. Preserve intentional surface grain and repeating details. Add only fine detail "
            "implied by the source. Do not invent a new "
            "design, add objects, borders or a background, or just interpolate the original pixels. "
            "Save only the finished RGBA texture as improved.png in the working directory, exactly " +
            std::to_string(job->width) + " by " + std::to_string(job->height) +
            " pixels. Resize the generated edit to these exact dimensions before finishing. Preserve transparency "
            "using the source alpha as a guide. If image generation is unavailable, report failure.";
        *error = "Could not start Codex. Check that the Codex CLI is on PATH";
        if (!launch(*job, prompt))
            return nullptr;
        *error = nullptr;
        return job.release();
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Texture AI: %s\n", e.what());
        return nullptr;
    }
}

// Kernel locks are released if a game exits during a write. A second editor
// reads the newest ZIP under this lock before copying its other entries.
struct ArchiveLock {
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool acquire(fs::path path) {
        path += ".v64-edit.lock";
        handle = CreateFileW(path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr, OPEN_ALWAYS,
                             FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        return handle != INVALID_HANDLE_VALUE;
    }
    ~ArchiveLock() {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
#else
    int fd = -1;
    bool acquire(const fs::path& path) {
        fd = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        return fd >= 0 && !flock(fd, LOCK_EX | LOCK_NB);
    }
    ~ArchiveLock() {
        if (fd >= 0)
            close(fd);
    }
#endif
};

// miniz clones entry comments but emits an empty archive comment. Preserve the
// pack author's archive comment as well, using the ZIP end record's 16-bit length.
static bool copy_zip_comment(const fs::path& source, const fs::path& destination) {
    std::ifstream in(source, std::ios::binary | std::ios::ate);
    auto end = in.tellg();
    if (end < 22)
        return false;
    const auto size = static_cast<size_t>(std::min<std::streamoff>(end, 65535 + 22));
    std::vector<uint8_t> tail(size);
    in.seekg(end - static_cast<std::streamoff>(size));
    if (!in.read(reinterpret_cast<char *>(tail.data()), size))
        return false;
    for (size_t offset = size - 22;; --offset) {
        const uint8_t *record = tail.data() + offset;
        size_t length = record[20] | (unsigned(record[21]) << 8);
        if (!std::memcmp(record, "PK\x05\x06", 4) && offset + 22 + length == size) {
            if (!length)
                return true;
            std::fstream out(destination, std::ios::in | std::ios::out | std::ios::binary);
            out.seekp(-2, std::ios::end);
            out.write(reinterpret_cast<const char *>(record + 20), length + 2);
            out.close();
            return !out.fail();
        }
        if (!offset)
            return false;
    }
}

static const char *edit_archive(const char *zip, uint64_t key, const void *png, size_t size) {
    try {
        fs::path path = fs::absolute(fs::u8path(zip));
        if (fs::exists(path))
            path = fs::canonical(path);
        fs::create_directories(path.parent_path());
        ArchiveLock lock;
        if (!lock.acquire(path))
            return "Could not lock the ZIP for editing. Check permissions or retry";
        TemporaryDirectory staging;
        if (!staging.create(path.parent_path()))
            return "Could not create a temporary ZIP beside the texture pack";
        auto temporary = staging.path / "pack.zip";
        bool exists = fs::exists(path);
        mz_zip_archive reader{}, writer{};
        if (exists && !mz_zip_reader_init_file(&reader, path.u8string().c_str(), 0))
            return "Could not read the existing ZIP";
        const unsigned count = mz_zip_reader_get_num_files(&reader);
        char name[64];
        std::snprintf(name, sizeof(name), GLES2_AI_ENTRY, key);
        const int old = exists ? mz_zip_reader_locate_file(&reader, name, nullptr, MZ_ZIP_FLAG_CASE_SENSITIVE) : -1;
        if ((!png && old < 0) || count > 250000 || (png && old < 0 && count == 250000)) {
            mz_zip_reader_end(&reader);
            return "Entry missing or ZIP exceeds the texture pack limit";
        }
        bool ok = mz_zip_writer_init_file_v2(&writer, temporary.u8string().c_str(), 0,
                                             exists && mz_zip_is_zip64(&reader) ? MZ_ZIP_FLAG_WRITE_ZIP64 : 0);
        for (unsigned i = 0; ok && i < count; ++i) {
            char entry[sizeof(name)];
            unsigned length = mz_zip_reader_get_filename(&reader, i, entry, sizeof(entry));
            if (!length)
                ok = false;
            else if (length != std::strlen(name) + 1 || std::strcmp(entry, name))
                ok = mz_zip_writer_add_from_zip_reader(&writer, &reader, i);
        }
        if (ok && png)
            ok = mz_zip_writer_add_mem(&writer, name, png, size, 0);
        if (ok)
            ok = mz_zip_writer_finalize_archive(&writer);
        if (writer.m_pState)
            ok = mz_zip_writer_end(&writer) && ok;
        if (reader.m_pState)
            mz_zip_reader_end(&reader);
        if (!ok || (exists && !copy_zip_comment(path, temporary)))
            return "Could not write the updated ZIP";
        if (exists)
            fs::permissions(temporary, fs::status(path).permissions());
        fs::rename(temporary, path);
        return nullptr;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Texture ZIP: %s\n", e.what());
        return "Could not update the ZIP. Check its permissions and free space";
    }
}

const char *gles2_editor_remove_loose(const char *dir, uint64_t key) {
    char name[32];
    std::snprintf(name, sizeof(name), "%016" PRIx64 ".gles.png", key);
    try {
        if (fs::remove(fs::u8path(dir) / name))
            return nullptr;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Texture editor: %s\n", e.what());
    }
    return "Could not remove the local replacement";
}

const char *gles2_ai_remove(const char *zip, uint64_t key) {
    return edit_archive(zip, key, nullptr, 0);
}

bool gles2_ai_poll(Gles2AiJob *job, const char *zip, uint64_t key, Gles2PackImage *image, const char **error) {
#ifdef _WIN32
    if (WaitForSingleObject(job->process, 0) == WAIT_TIMEOUT)
        return false;
    DWORD code;
    bool ok = GetExitCodeProcess(job->process, &code) && code == 0;
#else
    int status;
    pid_t result = waitpid(job->pid, &status, WNOHANG);
    if (result == 0 || (result < 0 && errno == EINTR))
        return false;
    job->pid = -1;
    bool ok = result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
    *error = "Codex failed. Check CLI sign-in and image generation availability";
    if (!ok)
        return true;
    *error = "Codex did not produce a valid PNG at the requested dimensions";
    try {
        auto output = job->work.path / "improved.png";
        if (fs::symlink_status(output).type() != fs::file_type::regular)
            return true;
        auto size = fs::file_size(output);
        if (!size || size > GLES2_TEXTURE_MAX_PNG_BYTES)
            return true;
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        std::ifstream file(output, std::ios::binary);
        if (file.read(reinterpret_cast<char *>(bytes.data()), bytes.size()) &&
            gles2_texture_decode_png(bytes.data(), bytes.size(), job->width, job->height, GLES2_TEXTURE_MAX_SIDE,
                                     image) &&
            image->width == job->width && image->height == job->height)
            *error = edit_archive(zip, key, bytes.data(), bytes.size());
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Texture AI: %s\n", e.what());
    }
    return true;
}
