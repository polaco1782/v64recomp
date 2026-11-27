#include "texture_pack.h"
#include "editor_ai.h"
#include "rice.h"
#include "runtime.h"
#include "host.h"
#include <SDL.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#else
#include <dirent.h>
#endif
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "miniz.h"
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define NO_FILE UINT32_MAX
typedef struct ImageFiles {
    unsigned png, alpha;
    bool opaque;
} ImageFiles;
typedef struct RiceEntry {
    char key[22];
    ImageFiles files;
    unsigned rank;
} RiceEntry;
typedef struct Part {
    char *stem;
    char key[22];
    unsigned index, kind, priority;
} Part;
typedef struct Match {
    uint64_t hash;
    size_t source;
} Match;
typedef struct Override {
    uint64_t id, key;
    unsigned zip_index; /* NO_FILE for a loose PNG. */
    bool identified, save_mapping;
} Override;
struct Gles2TexturePack {
    mz_zip_archive zip;
#ifdef _WIN32
    FILE *zip_file;
#endif
    RiceEntry *entries;
    size_t count;
    RiceState *rice;
    char *overrides, *zip_path;
    Override *files, **by_key;
    size_t file_count, file_capacity, unidentified;
    Match *matches;
    size_t match_count, match_capacity;
};
static bool hex_token(const char *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!SDL_isxdigit((unsigned char)p[i]))
            return false;
    return true;
}
static bool rice_crc(const char **cursor) {
    const char *p = *cursor;
    if (*p == '$')
        ++p;
    else if (strlen(p) >= 8 && hex_token(p, 8))
        p += 8;
    else
        return false;
    *cursor = p;
    return true;
}
static bool rice_key(const char *p) {
    if (!rice_crc(&p) || *p++ != '#' || *p < '0' || *p++ > '4' || *p++ != '#' || *p < '0' || *p++ > '3')
        return false;
    if (*p == '#') {
        ++p;
        if (!rice_crc(&p))
            return false;
    }
    return !*p;
}
static bool parse_part(char *path, const char *title, Part *part) {
    char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    for (char *p = base; *p; ++p)
        if (*p >= 'A' && *p <= 'Z')
            *p += (char)('a' - 'A');
    char *name = base, *hash = strchr(base, '#'), *suffix = strrchr(base, '_');
    if (!hash || !suffix || suffix <= hash)
        return false;
    bool alternate = false;
    while (!strncmp(name, "copy of", 7) || !strncmp(name, "flash", 5)) {
        char *space = name + (*name == 'c' ? 7 : 5);
        if (!SDL_isspace((unsigned char)*space))
            break;
        do
            ++space;
        while (SDL_isspace((unsigned char)*space));
        name = space;
        alternate = true;
    }
    if (hash < name || (size_t)(hash - name) != strlen(title) || SDL_strncasecmp(name, title, (size_t)(hash - name)))
        return false;
    const char *kinds[] = {"_a.png", "_rgb.png", "_allcibyrgba.png", "_cibyrgba.png", "_all.png"};
    unsigned kind = 0;
    while (kind < 5 && strcmp(suffix, kinds[kind]))
        ++kind;
    if (kind == 5)
        return false;
    *suffix = 0;
    if (!rice_key(hash + 1))
        return false;
    part->stem = SDL_strdup(path);
    if (!part->stem)
        return false;
    SDL_strlcpy(part->key, hash + 1, sizeof(part->key));
    part->kind = kind;
    const char edits[] = "v64recomp/overrides/rice/";
    part->priority =
        (base - path == (ptrdiff_t)(sizeof(edits) - 1) && !strncmp(path, edits, sizeof(edits) - 1) ? 8u : 0u) +
        (alternate ? 0u : 4u);
    return true;
}
static int by_stem(const void *a, const void *b) {
    return strcmp(((const Part *)a)->stem, ((const Part *)b)->stem);
}
static int by_key(const void *a, const void *b) {
    return strcmp(((const RiceEntry *)a)->key, ((const RiceEntry *)b)->key);
}
static void add_override(Gles2TexturePack *p, const char *name, unsigned zip_index) {
    if (strlen(name) != 25 || strcmp(name + 16, ".gles.png") || !hex_token(name, 16))
        return;
    if (zip_index != NO_FILE && strspn(name, "0123456789abcdef") != 16)
        return;
    if (p->file_count == p->file_capacity) {
        size_t cap = p->file_capacity ? p->file_capacity * 2 : 16;
        Override *files = realloc(p->files, cap * sizeof(*files));
        if (!files)
            return;
        p->files = files;
        p->file_capacity = cap;
    }
    uint64_t id = strtoull(name, NULL, 16);
    p->files[p->file_count++] = (Override){.id = id, .key = id, .zip_index = zip_index};
}
static bool index_zip(Gles2TexturePack *p, const char *title) {
    unsigned total = mz_zip_reader_get_num_files(&p->zip);
    if (!total || total > 250000)
        return false;
    Part *parts = calloc(total, sizeof(*parts));
    if (!parts)
        return false;
    unsigned count = 0;
    for (unsigned i = 0; i < total; i++) {
        char path[4096];
        unsigned n = mz_zip_reader_get_filename(&p->zip, i, NULL, 0);
        if (n < 2 || n > sizeof(path) || !mz_zip_reader_get_filename(&p->zip, i, path, sizeof(path)) ||
            strlen(path) != n - 1)
            continue;
        if (!strncmp(path, GLES2_AI_PREFIX, sizeof(GLES2_AI_PREFIX) - 1)) {
            add_override(p, path + sizeof(GLES2_AI_PREFIX) - 1, i);
            continue;
        }
        for (char *c = path; *c; ++c)
            if (*c == '\\')
                *c = '/';
        if (parse_part(path, title, &parts[count]))
            parts[count++].index = i;
    }
    qsort(parts, count, sizeof(*parts), by_stem);
    p->entries = calloc(count, sizeof(*p->entries));
    if (p->entries)
        for (unsigned i = 0; i < count;) {
            unsigned slots[5] = {0}, files[5] = {0};
            unsigned start = i;
            do {
                unsigned kind = parts[i].kind;
                slots[kind]++;
                files[kind] = parts[i].index;
                ++i;
            } while (i < count && !strcmp(parts[start].stem, parts[i].stem));
            unsigned kind = 4;
            while (kind && !slots[kind])
                --kind;
            bool pair = slots[0] && slots[1];
            if (pair)
                kind = 1;
            if (!kind)
                continue;
            RiceEntry *e = &p->entries[p->count++];
            SDL_strlcpy(e->key, parts[start].key, sizeof(e->key));
            e->files = (ImageFiles){slots[kind] == 1 && (!pair || slots[0] == 1) ? files[kind] : NO_FILE,
                                    pair ? files[0] : NO_FILE, kind == 1 && !pair};
            e->rank = parts[start].priority + (pair ? 3 : kind == 1 ? 1 : 2);
        }
    for (unsigned i = 0; i < count; i++)
        SDL_free(parts[i].stem);
    free(parts);
    if (!p->count)
        return false;
    qsort(p->entries, p->count, sizeof(*p->entries), by_key);
    size_t kept = 0;
    for (size_t i = 0; i < p->count;) {
        RiceEntry best = p->entries[i++];
        while (i < p->count && !strcmp(best.key, p->entries[i].key)) {
            if (p->entries[i].rank > best.rank)
                best = p->entries[i];
            else if (p->entries[i].rank == best.rank)
                best.files.png = NO_FILE;
            ++i;
        }
        p->entries[kept++] = best;
    }
    p->count = kept;
    return true;
}
static RiceEntry *find_key(Gles2TexturePack *p, const char *key) {
    if (!*key)
        return NULL;
    RiceEntry probe = {0};
    SDL_strlcpy(probe.key, key, sizeof(probe.key));
    return bsearch(&probe, p->entries, p->count, sizeof(*p->entries), by_key);
}
static RiceEntry *find_rice(Gles2TexturePack *p, char keys[2][22]) {
    for (unsigned i = 0; i < 2; ++i) {
        RiceEntry *e = find_key(p, keys[i]);
        if (e)
            return e;
    }
    for (unsigned i = 0; i < 2; ++i) {
        if (strlen(keys[i]) != 21 || strncmp(keys[i] + 8, "#2#", 3))
            continue;
        char key[22];
        memcpy(key, keys[i], 13);
        strcpy(key + 13, "$");
        RiceEntry *e = find_key(p, key);
        if (e)
            return e;
        snprintf(key, sizeof(key), "$%s", keys[i] + 8);
        e = find_key(p, key);
        if (e)
            return e;
    }
    return NULL;
}
static void index_local(Gles2TexturePack *p, const char *dir) {
#ifdef _WIN32
    char *pattern = NULL;
    if (SDL_asprintf(&pattern, "%s/*", dir) < 0)
        return;
    WCHAR *wide = (WCHAR *)SDL_iconv_string("UTF-16LE", "UTF-8", pattern, strlen(pattern) + 1);
    SDL_free(pattern);
    if (!wide)
        return;
    WIN32_FIND_DATAW entry;
    HANDLE it = FindFirstFileW(wide, &entry);
    SDL_free(wide);
    if (it == INVALID_HANDLE_VALUE)
        return;
    do {
        char name[32];
        if (WideCharToMultiByte(CP_UTF8, 0, entry.cFileName, -1, name, sizeof(name), NULL, NULL))
            add_override(p, name, NO_FILE);
    } while (FindNextFileW(it, &entry));
    FindClose(it);
#else
    DIR *it = opendir(dir);
    if (!it)
        return;
    struct dirent *entry;
    while ((entry = readdir(it)))
        add_override(p, entry->d_name, NO_FILE);
    closedir(it);
#endif
}

static int by_stored_id(const void *a, const void *b) {
    const Override *x = a, *y = b;
    return (x->id > y->id) - (x->id < y->id);
}
static int by_tile_key(const void *a, const void *b) {
    const Override *x = *(Override *const *)a, *y = *(Override *const *)b;
    if (x->key != y->key)
        return (x->key > y->key) - (x->key < y->key);
    bool loose_x = x->zip_index == NO_FILE, loose_y = y->zip_index == NO_FILE;
    if (loose_x != loose_y)
        return loose_x ? 1 : -1;
    /* Prefer the canonical filename, then the last AI edit in archive order.
     * Loose files have no archive order, so break ties by stored ID. */
    if ((x->id == x->key) != (y->id == y->key))
        return x->id == x->key ? -1 : 1;
    if (x->zip_index != y->zip_index)
        return x->zip_index > y->zip_index ? -1 : 1;
    return by_stored_id(y, x);
}
static char *alias_path(const Gles2TexturePack *p, uint64_t id) {
    char *path = NULL;
    if (p->overrides)
        SDL_asprintf(&path, "%s/%016" PRIx64 ".tile-id", p->overrides, id);
    return path;
}
static bool parse_mapping(const char *text, uint64_t *key) {
    if (text[16] != '\n' || !hex_token(text, 16))
        return false;
    *key = strtoull(text, NULL, 16);
    return true;
}
static bool read_zip_mapping(Gles2TexturePack *p, Override *file) {
    if (!p->zip.m_pState)
        return false;
    char name[64], text[18] = {0};
    snprintf(name, sizeof(name), GLES2_TILE_ID_ENTRY, file->id);
    int index = mz_zip_reader_locate_file(&p->zip, name, NULL, 0);
    mz_zip_archive_file_stat stat;
    return index >= 0 && mz_zip_reader_file_stat(&p->zip, (unsigned)index, &stat) && stat.m_uncomp_size == 17 &&
           mz_zip_reader_extract_to_mem(&p->zip, (unsigned)index, text, 17, 0) && parse_mapping(text, &file->key);
}
static void index_overrides(Gles2TexturePack *p) {
    if (!p->file_count)
        return;
    p->by_key = malloc(p->file_count * sizeof(*p->by_key));
    if (!p->by_key) {
        p->file_count = 0;
        return;
    }
    qsort(p->files, p->file_count, sizeof(*p->files), by_stored_id);
    for (size_t i = 0; i < p->file_count; ++i) {
        Override *file = &p->files[i];
        if (i && file[-1].id == file->id) {
            file->key = file[-1].key;
            file->identified = file[-1].identified;
        } else if (read_zip_mapping(p, file)) {
            file->identified = true;
        } else {
            char *path = alias_path(p, file->id);
            SDL_RWops *input = path ? SDL_RWFromFile(path, "rb") : NULL;
            SDL_free(path);
            if (input) {
                char text[18] = {0};
                if (SDL_RWread(input, text, 1, sizeof(text)) == 17 && parse_mapping(text, &file->key)) {
                    file->save_mapping = true;
                    file->identified = true;
                }
                SDL_RWclose(input);
            }
        }
        p->unidentified += !file->identified;
        p->by_key[i] = file;
    }
    qsort(p->by_key, p->file_count, sizeof(*p->by_key), by_tile_key);
}
static bool open_archive(Gles2TexturePack *p, const char *path) {
#ifdef _WIN32
    /* Permit the editor to atomically replace the archive while this reader still uses its snapshot. */
    wchar_t *wide = recomp_wide_path(path);
    HANDLE handle = wide ? CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                       OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL)
                         : INVALID_HANDLE_VALUE;
    free(wide);
    if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        errno = error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ? ENOENT : EACCES;
        return false;
    }
    int fd = _open_osfhandle((intptr_t)handle, _O_RDONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(handle);
        return false;
    }
    p->zip_file = _fdopen(fd, "rb");
    if (!p->zip_file) {
        _close(fd);
        return false;
    }
    return mz_zip_reader_init_cfile(&p->zip, p->zip_file, 0, 0);
#else
    return mz_zip_reader_init_file(&p->zip, path, 0);
#endif
}
Gles2TexturePack *gles2_texture_pack_open(const char *path, const char *overrides, const char *title) {
    if ((!path || !*path) && (!overrides || !*overrides))
        return NULL;
    Gles2TexturePack *p = calloc(1, sizeof(*p));
    if (!p)
        return NULL;
    if (overrides && *overrides) {
        p->overrides = SDL_strdup(overrides);
        if (p->overrides)
            index_local(p, overrides);
    }
    if (path && *path) {
        p->zip_path = SDL_strdup(path);
        if (open_archive(p, path)) {
            if (index_zip(p, title ? title : ""))
                p->rice = calloc(1, sizeof(*p->rice));
            fprintf(stderr, "GLES texture ZIP: %zu Rice IDs\n", p->count);
        } else if (getenv("RECOMP_TEXTURE_PACK") || errno != ENOENT)
            fprintf(stderr, "GLES could not open texture ZIP: %s\n", path);
    }
    index_overrides(p);
    return p;
}
bool gles2_texture_pack_enabled(const Gles2TexturePack *p) {
    return p && (p->rice || p->file_count);
}
bool gles2_texture_pack_has_rice(const Gles2TexturePack *p) {
    return p && p->rice;
}
void gles2_texture_pack_record_load(Gles2TexturePack *p, uint64_t serial, const Tile *tile, uint32_t w0, uint32_t w1,
                                    unsigned width, unsigned size, const TextureTransfer *dma, const uint8_t *ram,
                                    size_t len, uint32_t addr) {
    rice_record_load(p->rice, serial, tile, w0, w1, width, size, dma, ram, len, addr);
}
bool gles2_texture_decode_png(const uint8_t *bytes, size_t n, unsigned width, unsigned height, unsigned limit,
                              Gles2PackImage *image) {
    const uint8_t png[] = {137, 80, 78, 71, 13, 10, 26, 10};
    int w, h, channels;
    *image = (Gles2PackImage){0};
    limit = SDL_min(limit, (unsigned)GLES2_TEXTURE_MAX_SIDE);
    if (!width || !height || !bytes || n < sizeof(png) || n > GLES2_TEXTURE_MAX_PNG_BYTES ||
        memcmp(png, bytes, sizeof(png)) || !stbi_info_from_memory(bytes, (int)n, &w, &h, &channels) || w <= 0 ||
        h <= 0 || (unsigned)w > limit || (unsigned)h > limit || (uint64_t)w * height != (uint64_t)h * width)
        return false;
    image->pixels = stbi_load_from_memory(bytes, (int)n, &w, &h, &channels, 4);
    if (!image->pixels)
        return false;
    image->width = (unsigned)w;
    image->height = (unsigned)h;
    return true;
}
void gles2_texture_pack_free_image(Gles2PackImage *image) {
    stbi_image_free(image->pixels);
    *image = (Gles2PackImage){0};
}
static bool decode_entry(mz_zip_archive *zip, unsigned index, Gles2PackTile key, unsigned limit,
                         Gles2PackImage *image) {
    mz_zip_archive_file_stat stat;
    if (index == NO_FILE || !mz_zip_reader_file_stat(zip, index, &stat) || !stat.m_uncomp_size ||
        stat.m_uncomp_size > GLES2_TEXTURE_MAX_PNG_BYTES)
        return false;
    size_t n = 0;
    void *bytes = mz_zip_reader_extract_to_heap(zip, index, &n, 0);
    bool ok = gles2_texture_decode_png(bytes, n, key.width, key.height, limit, image);
    mz_free(bytes);
    return ok;
}
static bool decode_local(Gles2TexturePack *p, Gles2PackTile key, unsigned limit, Gles2PackImage *image) {
    char *path = NULL;
    if (SDL_asprintf(&path, "%s/%016" PRIx64 ".gles.png", p->overrides, key.hash) < 0)
        return false;
    SDL_RWops *file = SDL_RWFromFile(path, "rb");
    SDL_free(path);
    if (!file)
        return false;
    Sint64 size = SDL_RWsize(file);
    size_t n = 0;
    void *bytes = size > 0 && size <= GLES2_TEXTURE_MAX_PNG_BYTES ? SDL_LoadFile_RW(file, &n, 0) : NULL;
    SDL_RWclose(file);
    bool ok = gles2_texture_decode_png(bytes, n, key.width, key.height, limit, image);
    SDL_free(bytes);
    return ok;
}
static bool decode_rice(Gles2TexturePack *p, ImageFiles files, Gles2PackTile key, unsigned limit,
                        Gles2PackImage *image) {
    if (!decode_entry(&p->zip, files.png, key, limit, image))
        return false;
    Gles2PackImage alpha = {0};
    if (files.alpha != NO_FILE && (!decode_entry(&p->zip, files.alpha, key, limit, &alpha) ||
                                   alpha.width != image->width || alpha.height != image->height)) {
        gles2_texture_pack_free_image(image);
        gles2_texture_pack_free_image(&alpha);
        return false;
    }
    if (files.opaque || alpha.pixels)
        for (size_t i = 0; i < (size_t)image->width * image->height * 4; i += 4)
            image->pixels[i + 3] = alpha.pixels ? alpha.pixels[i] : 255;
    gles2_texture_pack_free_image(&alpha);
#ifndef NDEBUG
    char name[4096];
    mz_zip_reader_get_filename(&p->zip, files.png, name, sizeof(name));
    fprintf(stderr, "GLES Rice loaded: %s (%ux%u)\n", name, image->width, image->height);
#endif
    return true;
}
/* Batch discoveries outside the draw path. The ZIP writer locks and reopens the latest
 * archive, including any PNGs the editor saved while this pack snapshot was open. */
void gles2_texture_pack_flush_mappings(Gles2TexturePack *p) {
    if (!p)
        return;
    size_t count = 0;
    for (size_t i = 0; i < p->file_count; ++i)
        count += p->files[i].save_mapping;
    if (!count)
        return;
    Gles2TileMapping *mappings = malloc(count * sizeof(*mappings));
    const char *error = mappings ? "No texture archive path" : "Could not allocate texture mappings";
    if (p->zip_path && mappings) {
        size_t n = 0;
        for (size_t i = 0; i < p->file_count; ++i)
            if (p->files[i].save_mapping)
                mappings[n++] = (Gles2TileMapping){p->files[i].id, p->files[i].key};
        error = gles2_archive_store_mappings(p->zip_path, mappings, count);
    }
    free(mappings);
    if (!error) {
        for (size_t i = 0; i < p->file_count; ++i)
            p->files[i].save_mapping = false;
        return;
    }
    SDL_Log("Could not save texture IDs in ZIP: %s. Trying the local overrides directory", error);
    for (size_t i = 0; i < p->file_count; ++i) {
        const Override *file = &p->files[i];
        if (!file->save_mapping)
            continue;
        char *path = alias_path(p, file->id);
        char text[18];
        snprintf(text, sizeof(text), "%016" PRIx64 "\n", file->key);
        if (!path || !recomp_create_directories(p->overrides) || !recomp_save_image(path, (const uint8_t *)text, 17))
            SDL_Log("Could not persist GLES texture ID for %016" PRIx64, file->id);
        SDL_free(path);
    }
}
void gles2_texture_pack_close(Gles2TexturePack *p) {
    if (!p)
        return;
    if (p->zip.m_pState)
        mz_zip_reader_end(&p->zip);
#ifdef _WIN32
    if (p->zip_file)
        fclose(p->zip_file);
#endif
    if (p->rice)
        for (unsigned i = 0; i < 512; ++i)
            free(p->rice->loads[i].bytes);
    gles2_texture_pack_flush_mappings(p);
    SDL_free(p->zip_path);
    free(p->rice);
    free(p->entries);
    free(p->files);
    free(p->by_key);
    free(p->matches);
    SDL_free(p->overrides);
    free(p);
}
static size_t match_position(const Gles2TexturePack *p, uint64_t hash) {
    size_t lo = 0, hi = p->match_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (p->matches[mid].hash < hash)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}
void gles2_texture_pack_remember(Gles2TexturePack *p, uint64_t hash, size_t source) {
    size_t i = match_position(p, hash);
    if (i < p->match_count && p->matches[i].hash == hash) {
        p->matches[i].source = source;
        return;
    }
    if (p->match_count == p->match_capacity) {
        size_t cap = p->match_capacity ? p->match_capacity * 2 : 64;
        if (cap > SIZE_MAX / sizeof(Match))
            return;
        Match *matches = realloc(p->matches, cap * sizeof(*matches));
        if (!matches)
            return;
        p->matches = matches;
        p->match_capacity = cap;
    }
    memmove(p->matches + i + 1, p->matches + i, (p->match_count - i) * sizeof(Match));
    p->matches[i] = (Match){hash, source};
    ++p->match_count;
}
bool gles2_texture_pack_known(const Gles2TexturePack *p, uint64_t hash, size_t *source) {
    size_t i = match_position(p, hash);
    if (i == p->match_count || p->matches[i].hash != hash)
        return false;
    *source = p->matches[i].source;
    return true;
}

size_t gles2_texture_pack_unidentified(const Gles2TexturePack *p) {
    return p ? p->unidentified : 0;
}
void gles2_texture_pack_identify(Gles2TexturePack *p, uint64_t stored, uint64_t key) {
    if (!p || !p->unidentified)
        return;
    Override probe = {.id = stored};
    Override *file = bsearch(&probe, p->files, p->file_count, sizeof(probe), by_stored_id);
    if (!file || file->identified)
        return;
    while (file > p->files && file[-1].id == stored)
        --file;
    file->save_mapping = key != stored;
    do {
        file->key = key;
        file->identified = true;
        --p->unidentified;
        ++file;
    } while (file != p->files + p->file_count && file->id == stored);
    if (key == stored)
        return;
    qsort(p->by_key, p->file_count, sizeof(*p->by_key), by_tile_key);
    /* A native or Rice match may already be resident when an old override is first recognized. */
    size_t i = match_position(p, key);
    if (i < p->match_count && p->matches[i].hash == key) {
        --p->match_count;
        memmove(p->matches + i, p->matches + i + 1, (p->match_count - i) * sizeof(Match));
    }
}
static bool find_override(const Gles2TexturePack *p, uint64_t key, bool loose, size_t *source) {
    if (!p)
        return false;
    size_t lo = 0, hi = p->file_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const Override *f = p->by_key[mid];
        if (f->key < key || (f->key == key && loose && f->zip_index != NO_FILE))
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == p->file_count)
        return false;
    const Override *f = p->by_key[lo];
    if (f->key != key || (f->zip_index == NO_FILE) != loose)
        return false;
    *source = p->count + (size_t)(f - p->files);
    return true;
}
uint64_t gles2_texture_pack_stored_id(const Gles2TexturePack *p, size_t source) {
    return p->files[source - p->count].id;
}
bool gles2_texture_pack_ai(Gles2TexturePack *p, uint64_t hash, size_t *source) {
    return find_override(p, hash, false, source);
}
bool gles2_texture_pack_local(const Gles2TexturePack *p, uint64_t hash, size_t *source) {
    return find_override(p, hash, true, source);
}
Gles2PackStatus gles2_texture_pack_rice(Gles2TexturePack *p, Gles2PackTile key, const Tile *tile, unsigned tlut,
                                        const Gles2RiceWord words[512], size_t *source) {
    if (!p->rice)
        return GLES2_PACK_MISS;
    char keys[2][22];
    if (!rice_keys(p->rice, tile, tlut, words, key.width, key.height, keys[0], keys[1]))
        return GLES2_PACK_INCOMPLETE;
    RiceEntry *entry = find_rice(p, keys);
    if (!entry || entry->files.png == NO_FILE)
        return GLES2_PACK_MISS;
    *source = (size_t)(entry - p->entries);
    return GLES2_PACK_FOUND;
}

bool gles2_texture_pack_decode(Gles2TexturePack *p, size_t source, Gles2PackTile key, unsigned limit,
                               Gles2PackImage *image) {
    if (source < p->count)
        return decode_rice(p, p->entries[source].files, key, limit, image);
    const Override *file = &p->files[source - p->count];
    if (file->zip_index != NO_FILE)
        return decode_entry(&p->zip, file->zip_index, key, limit, image);
    key.hash = file->id;
    return decode_local(p, key, limit, image);
}
