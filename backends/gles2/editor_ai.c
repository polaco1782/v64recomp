#define _XOPEN_SOURCE 700
#define _FILE_OFFSET_BITS 64
#include "editor_ai.h"
#include "host.h"
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <wchar.h>
#include <windows.h>
typedef wchar_t PathChar;
#define NATIVE(s) L##s
#define path_length wcslen
#define path_copy _wcsdup
#define path_open _wfopen
#define file_seek _fseeki64
#define path_unlink _wunlink
#define path_rmdir _wrmdir
#define path_mkdir(p) _wmkdir(p)
#define native_path recomp_wide_path
#define SEPARATOR L'\\'
#else
#include <fcntl.h>
#include <ftw.h>
#include <signal.h>
#include <spawn.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
typedef char PathChar;
#define NATIVE(s) s
#define path_length strlen
#define path_copy strdup
#define path_open fopen
#define file_seek fseeko
#define path_unlink unlink
#define path_rmdir rmdir
#define path_mkdir(p) mkdir(p, 0755)
#define native_path strdup
#define SEPARATOR '/'
#endif

struct Gles2AiJob {
    PathChar *directory;
    unsigned width, height;
#ifdef _WIN32
    HANDLE group, process;
#else
    pid_t pid;
#endif
};

static PathChar *join_path(const PathChar *dir, const PathChar *name) {
    size_t base = path_length(dir), suffix = path_length(name);
    PathChar *path = malloc((base + suffix + 2) * sizeof(*path));
    if (path) {
        memcpy(path, dir, base * sizeof(*path));
        if (base && path[base - 1] != SEPARATOR)
            path[base++] = SEPARATOR;
        memcpy(path + base, name, (suffix + 1) * sizeof(*path));
    }
    return path;
}

/* Archive paths are normalized on Windows before splitting them. */
static PathChar *parent_path(const PathChar *path) {
    size_t end = path_length(path);
    while (end && path[end - 1] != SEPARATOR)
        --end;
    if (!end)
        return path_copy(NATIVE("."));
    size_t root = 1;
#ifdef _WIN32
    if (end >= 2 && path[end - 2] == L':')
        root = end; /* Keep the separator in C:\ and extended drive roots. */
#endif
    PathChar *parent = path_copy(path);
    if (parent) {
        if (end > root)
            --end;
        parent[end] = 0;
    }
    return parent;
}

static bool make_directories(const PathChar *dir) {
#ifdef _WIN32
    DWORD attrs = GetFileAttributesW(dir);
    if (attrs != INVALID_FILE_ATTRIBUTES)
        return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
#endif
    if (!path_mkdir(dir) || errno == EEXIST)
        return true;
    if (errno != ENOENT)
        return false;
    PathChar *parent = parent_path(dir);
    bool ok = parent && path_length(parent) < path_length(dir) && make_directories(parent);
    free(parent);
    return ok && (!path_mkdir(dir) || errno == EEXIST);
}

static PathChar *temporary_directory(const PathChar *parent) {
#ifdef _WIN32
    for (unsigned i = 0; i < 100; ++i) {
        wchar_t name[64];
        swprintf(name, 64, L"v64-texture-%lu-%llu-%u", GetCurrentProcessId(), (unsigned long long)GetTickCount64(), i);
        wchar_t *path = join_path(parent, name);
        if (!path || CreateDirectoryW(path, NULL))
            return path;
        DWORD error = GetLastError();
        free(path);
        if (error != ERROR_ALREADY_EXISTS)
            break;
    }
    return NULL;
#else
    char *pattern = join_path(parent, "v64-texture-XXXXXX");
    if (pattern && mkdtemp(pattern))
        return pattern;
    free(pattern);
    return NULL;
#endif
}

static PathChar *create_workspace(void) {
#ifdef _WIN32
    wchar_t parent[MAX_PATH + 1];
    DWORD length = GetTempPathW(MAX_PATH + 1, parent);
    return length && length <= MAX_PATH ? temporary_directory(parent) : NULL;
#else
    const char *variables[] = {"TMPDIR", "TMP", "TEMP", "TEMPDIR"};
    const char *parent = "/tmp";
    for (size_t i = 0; i < sizeof(variables) / sizeof(*variables); ++i) {
        const char *value = getenv(variables[i]);
        if (value && *value) {
            parent = value;
            break;
        }
    }
    char *absolute = realpath(parent, NULL);
    char *work = absolute ? temporary_directory(absolute) : NULL;
    free(absolute);
    return work;
#endif
}

#ifdef _WIN32
static void remove_workspace(const wchar_t *dir) {
    DWORD attrs = GetFileAttributesW(dir);
    if (attrs == INVALID_FILE_ATTRIBUTES)
        return;
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        DeleteFileW(dir);
        return;
    }
    /* A junction or directory symlink is itself an entry to remove. */
    if (!(attrs & FILE_ATTRIBUTE_REPARSE_POINT)) {
        wchar_t *pattern = join_path(dir, L"*");
        WIN32_FIND_DATAW entry;
        HANDLE search = pattern ? FindFirstFileW(pattern, &entry) : INVALID_HANDLE_VALUE;
        free(pattern);
        if (search != INVALID_HANDLE_VALUE) {
            do {
                if (!wcscmp(entry.cFileName, L".") || !wcscmp(entry.cFileName, L".."))
                    continue;
                wchar_t *child = join_path(dir, entry.cFileName);
                if (child)
                    remove_workspace(child);
                free(child);
            } while (FindNextFileW(search, &entry));
            FindClose(search);
        }
    }
    RemoveDirectoryW(dir);
}
#else
static int remove_entry(const char *path, const struct stat *info, int type, struct FTW *walk) {
    (void)info;
    (void)type;
    (void)walk;
    return remove(path);
}
static void remove_workspace(const char *dir) {
    nftw(dir, remove_entry, 16, FTW_DEPTH | FTW_PHYS);
}
#endif

static bool launch(Gles2AiJob *job, PathChar *source, char *prompt) {
#ifdef _WIN32
    /* Only the two workspace paths vary. Neither ends in a backslash or can
     * contain a double quote. The editor supplies the entire prompt below. */
    const wchar_t format[] =
        L"codex exec --ephemeral --sandbox workspace-write --skip-git-repo-check -C \"%ls\" --image \"%ls\" -- \"%ls\"";
    wchar_t *wide_prompt = recomp_wide_path(prompt);
    if (!wide_prompt)
        return false;
    size_t length = wcslen(format) + wcslen(job->directory) + wcslen(source) + wcslen(wide_prompt) + 1;
    wchar_t *command = malloc(length * sizeof(*command));
    bool ok = command && swprintf(command, length, format, job->directory, source, wide_prompt) >= 0;
    free(wide_prompt);

    job->group = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {0};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    STARTUPINFOW startup = {0};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION child = {0};
    ok = ok && job->group &&
         SetInformationJobObject(job->group, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) &&
         CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, NULL, job->directory,
                        &startup, &child);
    free(command);
    if (ok) {
        job->process = child.hProcess;
        ok = AssignProcessToJobObject(job->group, child.hProcess) && ResumeThread(child.hThread) != (DWORD)-1;
        if (!ok)
            TerminateProcess(child.hProcess, 1);
        CloseHandle(child.hThread);
    }
    return ok;
#else
    char *args[] = {"codex",
                    "exec",
                    "--ephemeral",
                    "--sandbox",
                    "workspace-write",
                    "--skip-git-repo-check",
                    "-C",
                    job->directory,
                    "--image",
                    source,
                    "--",
                    prompt,
                    NULL};
    posix_spawnattr_t attr;
    if (posix_spawnattr_init(&attr))
        return false;
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions)) {
        posix_spawnattr_destroy(&attr);
        return false;
    }
    /* The CLI reads piped stdin in addition to its prompt. */
    pid_t child;
    bool ok = !posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP) && !posix_spawnattr_setpgroup(&attr, 0) &&
              !posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0) &&
              !posix_spawnp(&child, args[0], &actions, &attr, args, environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    if (ok)
        job->pid = child;
    return ok;
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
        while (waitpid(job->pid, NULL, 0) < 0 && errno == EINTR) {
        }
    }
#endif
    if (job->directory)
        remove_workspace(job->directory);
    free(job->directory);
    free(job);
}

static bool write_source_png(const PathChar *source, const uint8_t *rgba, unsigned width, unsigned height) {
    FILE *file = path_open(source, NATIVE("wb"));
    if (!file)
        return false;
    size_t size = 0;
    void *png = tdefl_write_image_to_png_file_in_memory(rgba, width, height, 4, &size);
    bool ok = png && fwrite(png, 1, size, file) == size;
    mz_free(png);
    return !fclose(file) && ok;
}

Gles2AiJob *gles2_ai_start(const uint8_t *rgba, unsigned width, unsigned height, const char **error) {
    *error = "Could not prepare the Codex workspace";
    if (!rgba || !width || !height || width > GLES2_TEXTURE_MAX_SIDE / GLES2_AI_SCALE ||
        height > GLES2_TEXTURE_MAX_SIDE / GLES2_AI_SCALE)
        return NULL;
    Gles2AiJob *job = calloc(1, sizeof(*job));
    if (!job)
        return NULL;
    job->width = width * GLES2_AI_SCALE;
    job->height = height * GLES2_AI_SCALE;
    job->directory = create_workspace();
    PathChar *source = job->directory ? join_path(job->directory, NATIVE("source.png")) : NULL;
    if (source && write_source_png(source, rgba, width, height)) {
        static const char format[] =
            "Use image generation to edit the attached source.png, a single N64 game texture. "
            "Recreate this exact pattern at higher resolution. Preserve every motif, its position, scale, "
            "orientation, "
            "repeat count and spacing, edge-to-edge tiling, palette, shading and alpha silhouette. Keep lettering "
            "and symbols legible and faithful. For markings stored upside down or mirrored, inspect a flipped "
            "view to identify the shapes and deliver the edit in the original stored orientation. Reconstruct "
            "smooth contours and consistent strokes for printed markings. Use the native tile as a layout "
            "reference. Remove pixelation artifacts: blocky texel edges, stair-stepped diagonals, color banding "
            "and low-resolution dithering. Keep edges crisp and gradients smooth. Avoid sharpening halos and "
            "per-texel color matching. Preserve intentional surface grain and repeating details. Add only fine "
            "detail "
            "implied by the source. Do not invent a new "
            "design, add objects, borders or a background, or just interpolate the original pixels. "
            "Save only the finished RGBA texture as improved.png in the working directory, exactly %u by %u"
            " pixels. Resize the generated edit to these exact dimensions before finishing. Preserve transparency "
            "using the source alpha as a guide. If image generation is unavailable, report failure.";
        char prompt[sizeof(format) + 24];
        snprintf(prompt, sizeof(prompt), format, job->width, job->height);
        *error = "Could not start Codex. Check that the Codex CLI is on PATH";
        if (launch(job, source, prompt))
            *error = NULL;
    }
    free(source);
    if (*error) {
        gles2_ai_destroy(job);
        return NULL;
    }
    return job;
}

/* Resolve an existing pack before locking and replacing it, so edits through a
 * symlink reach the same file as edits made through its target. */
static PathChar *archive_path(const char *zip) {
#ifdef _WIN32
    wchar_t *name = recomp_wide_path(zip);
    if (!name)
        return NULL;
    HANDLE file = CreateFileW(name, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              OPEN_EXISTING, 0, NULL);
    wchar_t *path = NULL;
    if (file == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
            path = _wfullpath(NULL, name, 0);
    } else {
        DWORD size = GetFinalPathNameByHandleW(file, NULL, 0, FILE_NAME_NORMALIZED);
        path = size ? malloc(size * sizeof(*path)) : NULL;
        if (path) {
            DWORD length = GetFinalPathNameByHandleW(file, path, size, FILE_NAME_NORMALIZED);
            if (!length || length >= size) {
                free(path);
                path = NULL;
            }
        }
        CloseHandle(file);
    }
    free(name);
    return path;
#else
    char *path = realpath(zip, NULL);
    return !path && errno == ENOENT ? strdup(zip) : path;
#endif
}

/* Kernel locks are released even when an editor exits during a write. */
#ifdef _WIN32
typedef HANDLE ArchiveLock;
#define INVALID_LOCK INVALID_HANDLE_VALUE
static ArchiveLock lock_archive(const wchar_t *path, const wchar_t *parent) {
    (void)parent;
    const wchar_t suffix[] = L".v64-edit.lock";
    size_t length = wcslen(path);
    wchar_t *lock = malloc(length * sizeof(*lock) + sizeof(suffix));
    if (!lock)
        return INVALID_LOCK;
    memcpy(lock, path, length * sizeof(*lock));
    memcpy(lock + length, suffix, sizeof(suffix));
    HANDLE handle = CreateFileW(lock, GENERIC_WRITE | DELETE, 0, NULL, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, NULL);
    free(lock);
    return handle;
}
#else
typedef int ArchiveLock;
#define INVALID_LOCK (-1)
static ArchiveLock lock_archive(const char *path, const char *parent) {
    (void)path;
    int fd = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0 && flock(fd, LOCK_EX | LOCK_NB)) {
        close(fd);
        return INVALID_LOCK;
    }
    return fd;
}
#endif

/* miniz copies entry comments, but writes an empty archive comment. The ZIP
 * end record and its 16-bit comment both fit in the last 65557 bytes. */
static bool copy_zip_comment(mz_zip_archive *reader, FILE *out) {
    uint64_t size = mz_zip_get_archive_size(reader);
    if (size < 22)
        return false;
    size_t count = size > 65535 + 22 ? 65535 + 22 : (size_t)size;
    uint8_t *tail = malloc(count);
    bool ok = false;
    if (tail && mz_zip_read_archive_data(reader, size - count, tail, count) == count) {
        for (size_t end = count; end >= 22; --end) {
            const uint8_t *record = tail + end - 22;
            unsigned length = record[20] | ((unsigned)record[21] << 8);
            if (end + length == count && !memcmp(record, "PK\x05\x06", 4)) {
                ok =
                    !length || (!file_seek(out, -2, SEEK_END) && fwrite(record + 20, 1, length + 2, out) == length + 2);
                break;
            }
        }
    }
    free(tail);
    return ok;
}

typedef struct ArchiveEdit {
    char name[64];
    const void *data; /* NULL removes the entry. */
    size_t size;
} ArchiveEdit;
static int compare_edits(const void *a, const void *b) {
    return strcmp(((const ArchiveEdit *)a)->name, ((const ArchiveEdit *)b)->name);
}
static const char *write_archive(FILE *input, FILE *output, const ArchiveEdit *edits, size_t edit_count) {
    mz_zip_archive reader = {0}, writer = {0};
    if (input && !mz_zip_reader_init_cfile(&reader, input, 0, 0))
        return "Could not read the existing ZIP";
    unsigned count = mz_zip_reader_get_num_files(&reader);
    const char *error = NULL;
    if (edit_count == 1 && !edits[0].data &&
        (!input || mz_zip_reader_locate_file(&reader, edits[0].name, NULL, MZ_ZIP_FLAG_CASE_SENSITIVE) < 0)) {
        error = "Entry missing from ZIP";
    } else if (count > 250000 || edit_count > 250000) {
        error = "ZIP exceeds the texture pack limit";
    } else {
        unsigned flags = input && mz_zip_is_zip64(&reader) ? MZ_ZIP_FLAG_WRITE_ZIP64 : 0;
        bool ok = mz_zip_writer_init_cfile(&writer, output, flags);
        unsigned kept = 0;
        for (unsigned i = 0; ok && i < count; ++i) {
            ArchiveEdit probe;
            unsigned length = mz_zip_reader_get_filename(&reader, i, NULL, 0);
            if (!length) {
                ok = false;
                break;
            }
            if (length <= sizeof(probe.name)) {
                if (!mz_zip_reader_get_filename(&reader, i, probe.name, sizeof(probe.name))) {
                    ok = false;
                    break;
                }
                if (strlen(probe.name) + 1 == length &&
                    bsearch(&probe, edits, edit_count, sizeof(*edits), compare_edits))
                    continue;
            }
            ok = mz_zip_writer_add_from_zip_reader(&writer, &reader, i);
            ++kept;
        }
        for (size_t i = 0; ok && i < edit_count; ++i) {
            const ArchiveEdit *e = &edits[i];
            if (e->data)
                ok = kept++ < 250000 && mz_zip_writer_add_mem(&writer, e->name, e->data, e->size, 0);
        }
        if (ok)
            ok = mz_zip_writer_finalize_archive(&writer);
        if (writer.m_pState)
            ok = mz_zip_writer_end(&writer) && ok;
        if (!error && (!ok || (input && !copy_zip_comment(&reader, output))))
            error = "Could not write the updated ZIP";
    }
    if (input)
        mz_zip_reader_end(&reader);
    return error;
}

static const char *edit_archive(const char *zip, const ArchiveEdit *edits, size_t count) {
    const char *update_error = "Could not update the ZIP. Check its permissions and free space";
    const char *error = update_error;
    PathChar *path = zip && *zip ? archive_path(zip) : NULL;
    PathChar *parent = path ? parent_path(path) : NULL;
    PathChar *staging = NULL, *temporary = NULL;
    ArchiveLock lock = INVALID_LOCK;
    FILE *input = NULL, *output = NULL;
    if (!parent || !make_directories(parent))
        goto done;
    lock = lock_archive(path, parent);
    if (lock == INVALID_LOCK) {
        error = "Could not lock the ZIP for editing. Check permissions or retry";
        goto done;
    }
    input = path_open(path, NATIVE("rb"));
    if (!input && errno != ENOENT) {
        error = "Could not read the existing ZIP";
        goto done;
    }
    staging = temporary_directory(parent);
    temporary = staging ? join_path(staging, NATIVE("pack.zip")) : NULL;
    output = temporary ? path_open(temporary, NATIVE("wb")) : NULL;
    if (!output) {
        error = "Could not create a temporary ZIP beside the texture pack";
        goto done;
    }
    error = write_archive(input, output, edits, count);
#ifndef _WIN32
    if (!error && input) {
        struct stat info;
        if (fflush(output) || fstat(fileno(input), &info) || fchmod(fileno(output), info.st_mode & 07777))
            error = update_error;
    }
#endif
    if (fclose(output))
        error = update_error;
    if (input) {
        fclose(input);
        input = NULL;
    }
#ifdef _WIN32
    if (!error && !MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        error = update_error;
#else
    if (!error && rename(temporary, path))
        error = update_error;
#endif
done:
    if (input)
        fclose(input);
    if (temporary)
        path_unlink(temporary);
    if (staging)
        path_rmdir(staging);
    if (lock != INVALID_LOCK) {
#ifdef _WIN32
        CloseHandle(lock);
#else
        close(lock);
#endif
    }
    free(temporary);
    free(staging);
    free(parent);
    free(path);
    return error;
}

const char *gles2_editor_remove_loose(const char *dir, uint64_t key) {
    char name[32];
    snprintf(name, sizeof(name), "%016" PRIx64 ".gles.png", key);
    PathChar *directory = native_path(dir);
    PathChar *filename = native_path(name);
    PathChar *path = directory && filename ? join_path(directory, filename) : NULL;
    bool ok = path && !path_unlink(path);
    free(path);
    free(filename);
    free(directory);
    return ok ? NULL : "Could not remove the local replacement";
}

const char *gles2_archive_store_mappings(const char *zip, const Gles2TileMapping *mappings, size_t count) {
    if (!count)
        return NULL;
    if (count > 250000)
        return "Too many texture IDs for the ZIP";
    ArchiveEdit *edits = calloc(count, sizeof(*edits));
    char (*values)[18] = calloc(count, sizeof(*values));
    const char *error = "Could not allocate ZIP mapping entries";
    if (edits && values) {
        for (size_t i = 0; i < count; ++i) {
            snprintf(edits[i].name, sizeof(edits[i].name), GLES2_TILE_ID_ENTRY, mappings[i].stored);
            snprintf(values[i], sizeof(values[i]), "%016" PRIx64 "\n", mappings[i].key);
            edits[i].data = values[i];
            edits[i].size = 17;
        }
        qsort(edits, count, sizeof(*edits), compare_edits);
        error = edit_archive(zip, edits, count);
    }
    free(values);
    free(edits);
    return error;
}
static const char *edit_ai_entry(const char *zip, uint64_t key, const void *png, size_t size) {
    ArchiveEdit edit = {.data = png, .size = size};
    snprintf(edit.name, sizeof(edit.name), GLES2_AI_ENTRY, key);
    return edit_archive(zip, &edit, 1);
}
const char *gles2_ai_remove(const char *zip, uint64_t key) {
    return edit_ai_entry(zip, key, NULL, 0);
}

/* Check the opened file itself and refuse links, pipes and oversized results. */
static FILE *open_result(const PathChar *path, size_t *size) {
#ifdef _WIN32
    HANDLE file =
        CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return NULL;
    BY_HANDLE_FILE_INFORMATION info;
    int fd = -1;
    if (GetFileType(file) == FILE_TYPE_DISK && GetFileInformationByHandle(file, &info) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) && !info.nFileSizeHigh &&
        info.nFileSizeLow && info.nFileSizeLow <= GLES2_TEXTURE_MAX_PNG_BYTES) {
        *size = info.nFileSizeLow;
        fd = _open_osfhandle((intptr_t)file, _O_RDONLY | _O_BINARY);
    }
    if (fd < 0) {
        CloseHandle(file);
        return NULL;
    }
    FILE *stream = _fdopen(fd, "rb");
    if (!stream)
        _close(fd);
#else
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return NULL;
    struct stat info;
    FILE *stream = NULL;
    if (!fstat(fd, &info) && S_ISREG(info.st_mode) && info.st_size > 0 && info.st_size <= GLES2_TEXTURE_MAX_PNG_BYTES) {
        *size = (size_t)info.st_size;
        stream = fdopen(fd, "rb");
    }
    if (!stream)
        close(fd);
#endif
    return stream;
}

bool gles2_ai_poll(Gles2AiJob *job, const char *zip, uint64_t key, Gles2PackImage *image, const char **error) {
#ifdef _WIN32
    DWORD waited = WaitForSingleObject(job->process, 0);
    if (waited == WAIT_TIMEOUT)
        return false;
    DWORD code;
    bool ok = waited == WAIT_OBJECT_0 && GetExitCodeProcess(job->process, &code) && code == 0;
#else
    int status;
    pid_t result = waitpid(job->pid, &status, WNOHANG);
    if (!result || (result < 0 && errno == EINTR))
        return false;
    job->pid = -1;
    bool ok = result > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0;
#endif
    if (!ok) {
        *error = "Codex failed. Check CLI sign-in and image generation availability";
        return true;
    }
    *error = "Codex did not produce a valid PNG at the requested dimensions";
    PathChar *output = join_path(job->directory, NATIVE("improved.png"));
    size_t size = 0;
    FILE *file = output ? open_result(output, &size) : NULL;
    free(output);
    if (file) {
        uint8_t *bytes = malloc(size);
        if (bytes && fread(bytes, 1, size, file) == size &&
            gles2_texture_decode_png(bytes, size, job->width, job->height, GLES2_TEXTURE_MAX_SIDE, image) &&
            image->width == job->width && image->height == job->height)
            *error = edit_ai_entry(zip, key, bytes, size);
        free(bytes);
        fclose(file);
    }
    return true;
}
