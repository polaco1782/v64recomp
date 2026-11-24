#include "cheat_database.h"
#include "host.h"
#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#else
#include <curl/curl.h>
#include <pthread.h>
#endif

enum { MAX_BYTES = 8 * 1024 * 1024, MAX_CHEATS = 4096 };
typedef struct {
    const char *key, *file;
} Source;
static const Source Project64Files[] = {
    {"30C7AC50-7704072D-C:45", "Conker's Bad Fur Day (U).cht"},
    {"04DAF07F-0D18E688-C:45", "Duke Nukem - ZERO HOUR (U).cht"},
    {"A273AB56-DA33DB9A-C:45", "Duke Nukem 64 (U).cht"},
    {"DCBC50D1-09FD1AA3-C:45", "GoldenEye 007 (U).cht"},
    {"9CAB6AEA-87C61C00-C:45", "Hexen (U).cht"},
    {"2857674D-CC4337DA-C:45", "Nightmare Creatures (U).cht"},
    {"693BA2AE-B7F14E9F-C:45", "The Legend of Zelda - Ocarina of Time (U) (V1.2).cht"},
    {"41F2B98F-B458B466-C:45", "Perfect Dark (U) (V1.1).cht"},
    {"9F5BF79C-D2FE08A0-C:45", "Quake 64 (U).cht"},
    {"BDA8F143-B1AF2D62-C:45", "Quake II (U).cht"},
    {"F3C5BF9B-160F33E2-C:45", "Rayman 2 - The Great Escape (U) (M5).cht"},
    {"3A4760B5-2D74D410-C:45", "Shadow Man (U).cht"},
    {"635A2BFF-8B022326-C:45", "Super Mario 64 (U).cht"},
};

#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE bool recomp_web_cheat_cancelled(const atomic_bool *cancel) {
    return atomic_load(cancel);
}
// Stream into a bounded Wasm buffer and abort
// the request on shutdown even when the server has stopped sending data.
// clang-format off
EM_ASYNC_JS(int, fetch_cheat_text, (const char *url, char *out, int capacity, const void *cancel), {
    const controller = new AbortController();
    const timeout = setTimeout(() => controller.abort(), 25000);
    const watch = setInterval(() => {
        if (_recomp_web_cheat_cancelled(cancel)) controller.abort();
    }, 100);
    try {
        const response = await fetch(UTF8ToString(url), {signal: controller.signal, credentials: 'omit', cache: 'no-cache'});
        if (!response.ok) return -response.status;
        const reader = response.body.getReader();
        let length = 0;
        for (;;) {
            const chunk = await reader.read();
            if (chunk.done) return length;
            if (chunk.value.length > capacity - length) return -1;
            HEAPU8.set(chunk.value, out + length);
            length += chunk.value.length;
        }
    } catch (e) {
        return -1;
    } finally {
        controller.abort();
        clearTimeout(timeout);
        clearInterval(watch);
    }
});
// clang-format on
static char *fetch(const char *url, const atomic_bool *cancel, char error[192], size_t *size) {
    char *bytes = malloc(MAX_BYTES + 1);
    if (!bytes)
        return NULL;
    int n = fetch_cheat_text(url, bytes, MAX_BYTES, cancel);
    if (n < 0 || atomic_load(cancel)) {
        if (n < -1)
            snprintf(error, 192, "HTTP %d", -n);
        else
            snprintf(error, 192, "%s", atomic_load(cancel) ? "Download cancelled" : "Browser download failed");
        free(bytes);
        return NULL;
    }
    bytes[n] = 0;
    *size = (size_t)n;
    return bytes;
}
#else
typedef struct {
    char *bytes;
    size_t count, capacity;
    const atomic_bool *cancel;
} Download;
static size_t receive(char *data, size_t size, size_t count, void *opaque) {
    Download *d = opaque;
    if (atomic_load(d->cancel) || (count && size > (MAX_BYTES - d->count) / count))
        return 0;
    size_t n = size * count;
    if (d->count + n + 1 > d->capacity) {
        size_t cap = SDL_max(d->capacity * 2, d->count + n + 1);
        cap = SDL_min(cap, MAX_BYTES + 1u);
        char *next = realloc(d->bytes, cap);
        if (!next)
            return 0;
        d->bytes = next;
        d->capacity = cap;
    }
    memcpy(d->bytes + d->count, data, n);
    d->count += n;
    d->bytes[d->count] = 0;
    return n;
}
static int progress(void *opaque, curl_off_t total, curl_off_t current, curl_off_t uploaded, curl_off_t sent) {
    (void)total;
    (void)current;
    (void)uploaded;
    (void)sent;
    return atomic_load(((Download *)opaque)->cancel);
}
static CURLcode curl_init_status;
static void init_curl(void) {
    curl_init_status = curl_global_init(CURL_GLOBAL_DEFAULT);
}
static char *fetch(const char *url, const atomic_bool *cancel, char error[192], size_t *size) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, init_curl);
    CURL *request = curl_init_status == CURLE_OK ? curl_easy_init() : NULL;
    if (!request) {
        snprintf(error, 192, "Cannot initialize HTTPS downloads");
        return NULL;
    }
    Download d = {.cancel = cancel};
    curl_easy_setopt(request, CURLOPT_URL, url);
    curl_easy_setopt(request, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(request, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(request, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(request, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(request, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(request, CURLOPT_CONNECTTIMEOUT, 8L);
    curl_easy_setopt(request, CURLOPT_TIMEOUT, 25L);
    curl_easy_setopt(request, CURLOPT_MAXFILESIZE_LARGE, (curl_off_t)MAX_BYTES);
    curl_easy_setopt(request, CURLOPT_USERAGENT, "v64recomp-cheats/1");
#ifdef _WIN32
    curl_easy_setopt(request, CURLOPT_SSL_OPTIONS, (long)CURLSSLOPT_NATIVE_CA);
#endif
    curl_easy_setopt(request, CURLOPT_WRITEFUNCTION, receive);
    curl_easy_setopt(request, CURLOPT_WRITEDATA, &d);
    curl_easy_setopt(request, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(request, CURLOPT_XFERINFODATA, &d);
    curl_easy_setopt(request, CURLOPT_NOPROGRESS, 0L);
    CURLcode result = curl_easy_perform(request);
    curl_easy_cleanup(request);
    if (result != CURLE_OK) {
        snprintf(error, 192, "HTTPS download failed: %s", curl_easy_strerror(result));
        free(d.bytes);
        return NULL;
    }
    *size = d.count;
    return d.bytes;
}
#endif

static char *read_cache(const char *path) {
    SDL_RWops *f = SDL_RWFromFile(path, "rb");
    if (!f)
        return NULL;
    Sint64 n = SDL_RWsize(f);
    char *s = n > 0 && n <= MAX_BYTES ? malloc((size_t)n + 1) : NULL;
    if (s && (SDL_RWread(f, s, 1, (size_t)n) != (size_t)n || memchr(s, 0, (size_t)n))) {
        free(s);
        s = NULL;
    }
    if (s)
        s[n] = 0;
    SDL_RWclose(f);
    return s;
}
static char *trim(char *s) {
    s += strspn(s, " \r\t\n");
    char *end = s + strlen(s);
    while (end > s && SDL_isspace((unsigned char)end[-1]))
        *--end = 0;
    return s;
}
static bool hex(const char *s, size_t n, uint32_t *out) {
    if (!n || n > 8)
        return false;
    uint32_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned c = (unsigned char)s[i];
        if (!SDL_isxdigit(c))
            return false;
        v = v * 16 + (c <= '9' ? c - '0' : (unsigned)SDL_toupper(c) - 'A' + 10);
    }
    *out = v;
    return true;
}
static void problem(Cheat *c, const char *s) {
    SDL_strlcpy(c->problem, s, sizeof(c->problem));
}
/* Grow bounded lists geometrically, using the element count to track capacity. */
static void *grow(void *ptr, size_t count, size_t element) {
    if (count && (count & (count - 1)))
        return ptr;
    return realloc(ptr, (count ? 2 * count : 1) * element);
}
static void add_option(Cheat *c, uint32_t v, const char *name) {
    if (c->option_count == 1024)
        return;
    CheatOption *items = grow(c->options, c->option_count, sizeof(*items));
    if (!items) {
        problem(c, "Out of memory");
        return;
    }
    c->options = items;
    items[c->option_count++] = (CheatOption){(uint16_t)v, name};
}
static void parse_line(Cheat *c, char *s, bool project) {
    size_t n = strcspn(s, " \t");
    char *value = s + n;
    value += strspn(value, " \t");
    size_t digits = strcspn(value, " \t");
    uint32_t address;
    if (!hex(s, n, &address) || n != 8 || digits != 4) {
        if (project && n <= 4 && *value && hex(s, n, &address))
            add_option(c, address, value);
        else
            problem(c, "Unrecognized code or option line");
        return;
    }
    CheatLine line = {.address = address};
    for (unsigned i = 0; i < 4; ++i) {
        uint32_t nibble;
        line.value <<= 4;
        line.mask <<= 4;
        if (value[i] == '?')
            line.mask |= 15;
        else if (hex(value + i, 1, &nibble))
            line.value |= (uint16_t)nibble;
        else {
            problem(c, "Invalid code value");
            return;
        }
    }
    if (c->line_count == 4096) {
        problem(c, "Too many code lines");
        return;
    }
    CheatLine *lines = grow(c->lines, c->line_count, sizeof(*lines));
    if (!lines) {
        problem(c, "Out of memory");
        return;
    }
    c->lines = lines;
    lines[c->line_count++] = line;
    char *rest = value + 4;
    rest += strspn(rest, " \t");
    while (*rest) {
        if (*rest == ',') {
            ++rest;
            rest += strspn(rest, " \t");
            if (!*rest)
                problem(c, "Unrecognized option syntax");
        }
        char *mark = strstr(rest, ":\"");
        uint32_t v;
        if (!mark || mark - rest > 4 || !hex(rest, (size_t)(mark - rest), &v))
            break;
        char *end = strchr(mark + 2, '"');
        if (!end)
            break;
        *end = 0;
        add_option(c, v, mark + 2);
        rest = end + 1;
        rest += strspn(rest, " \t");
    }
    if (*rest)
        problem(c, "Unrecognized option syntax");
}
static void free_cheat(Cheat *c) {
    free(c->note);
    free(c->program);
    free(c->options);
    free(c->lines);
}
void cheat_catalog_destroy(CheatCatalog *db) {
    for (size_t i = 0; i < db->count; ++i)
        free_cheat(&db->cheats[i]);
    free(db->cheats);
    free(db->sources[0]);
    free(db->sources[1]);
    *db = (CheatCatalog){0};
}
static uint64_t signature(const Cheat *c) {
    if (*c->problem || c->option_count || !c->line_count)
        return 0;
    uint64_t h = 1;
    for (size_t i = 0; i < c->line_count; ++i) {
        if (c->lines[i].mask)
            return 0;
        h = (h ^ c->lines[i].address) * UINT64_C(1099511628211);
        h = (h ^ c->lines[i].value) * UINT64_C(1099511628211);
    }
    return h ? h : 1;
}
static void finish(CheatCatalog *db, Cheat *c, uint64_t *seen) {
    if (!c->name)
        return;
    uint64_t sig = signature(c);
    bool duplicate = false;
    for (size_t i = 0; sig && i < db->count; ++i) {
        Cheat *other = &db->cheats[i];
        if (seen[i] == sig && c->line_count == other->line_count &&
            !memcmp(c->lines, other->lines, c->line_count * sizeof(*c->lines))) {
            duplicate = true;
            break;
        }
    }
    if (!duplicate && db->count < MAX_CHEATS) {
        Cheat *entries = grow(db->cheats, db->count, sizeof(*entries));
        if (entries) {
            db->cheats = entries;
            seen[db->count] = sig;
            db->cheats[db->count++] = *c;
            *c = (Cheat){0};
            return;
        }
        SDL_strlcat(db->status, "Out of memory\n", sizeof(db->status));
    }
    free_cheat(c);
    *c = (Cheat){0};
}
static unsigned parse(CheatCatalog *db, char *text, const char *key, const char *source, bool project, uint64_t *seen) {
    Cheat c = {0};
    bool matched = false;
    unsigned count = 0;
    while (text && *text) {
        char *next = strchr(text, '\n');
        if (next)
            *next++ = 0;
        char *s = trim(text);
        text = next;
        if (!*s || *s == '#' || !strncmp(s, "//", 2))
            continue;
        if (!strncmp(s, "crc ", 4) || (project && *s == '[')) {
            finish(db, &c, seen);
            if (*s == '[') {
                char *end = strrchr(s, ']');
                if (end && !end[1])
                    *end = 0;
                s++;
            } else
                s = trim(s + 4);
            matched = !SDL_strcasecmp(s, key);
        } else if (matched && ((!project && !strncmp(s, "cn ", 3)) || (project && *s == '$'))) {
            finish(db, &c, seen);
            if (count == MAX_CHEATS)
                break;
            ++count;
            c.name = s + (project ? 1 : 3);
            c.source = source;
        } else if (matched && c.name) {
            if ((!project && !strncmp(s, "cd ", 3)) || (project && !strncmp(s, "Note=", 5))) {
                const char *note = s + (project ? 5 : 3);
                size_t old = c.note ? strlen(c.note) : 0;
                char *p = realloc(c.note, old + strlen(note) + 2);
                if (!p) {
                    problem(&c, "Out of memory");
                    continue;
                }
                c.note = p;
                if (old)
                    p[old++] = '\n';
                strcpy(p + old, note);
            } else if (strncmp(s, "gn ", 3) && strncmp(s, "Name=", 5))
                parse_line(&c, s, project);
        }
    }
    finish(db, &c, seen);
    return count;
}
void cheat_compile(Cheat *c, const Runtime *rt) {
    if (*c->problem)
        return;
    if (!c->program && c->line_count)
        c->program = calloc(c->line_count, sizeof(*c->program));
    if (!c->program && c->line_count) {
        problem(c, "Out of memory");
        return;
    }
    for (size_t i = 0; i < c->line_count; ++i) {
        CheatLine l = c->lines[i];
        if (l.mask && !c->option_count) {
            problem(c, "Code requires an option value that the source did not provide");
            return;
        }
        uint16_t parameter = c->option_count ? c->options[c->option].value : 0;
        c->program[i] = (RecompCheatCode){l.address, (uint16_t)(l.value | (parameter & l.mask))};
    }
    recomp_cheats_validate(rt, c->program, c->line_count, c->problem, sizeof(c->problem));
}
void cheat_catalog_load(CheatCatalog *db, const char *key, const char *dir, bool refresh, const atomic_bool *cancel) {
    const char *mupen = "https://raw.githubusercontent.com/mupen64plus/mupen64plus-core/master/data/mupencheat.txt";
    char project[512] = "https://raw.githubusercontent.com/project64/project64/develop/Config/Cheats/";
    bool have_project = false;
    for (size_t i = 0; i < SDL_arraysize(Project64Files); ++i) {
        if (strcmp(Project64Files[i].key, key))
            continue;
        size_t n = strlen(project);
        for (const unsigned char *p = (const unsigned char *)Project64Files[i].file; *p; ++p) {
            if (SDL_isalnum(*p) || strchr("-_.~", *p))
                project[n++] = (char)*p;
            else
                n += (size_t)snprintf(project + n, 4, "%%%02X", *p);
        }
        project[n] = 0;
        have_project = true;
        break;
    }
    uint64_t *seen = calloc(MAX_CHEATS, sizeof(*seen));
    if (!seen) {
        SDL_strlcpy(db->status, "Out of memory", sizeof(db->status));
        return;
    }
    for (unsigned i = 0; i < (have_project ? 2u : 1u) && !atomic_load(cancel); ++i) {
        const char *name = i ? "Project64" : "Mupen64Plus";
        char path[4096], error[192] = "Out of memory";
        if (snprintf(path, sizeof(path), "%s/%s.txt", dir, name) >= (int)sizeof(path)) {
            SDL_strlcat(db->status, "Cache path too long\n", sizeof(db->status));
            break;
        }
        char *text = read_cache(path);
        const char *status = "cached";
        if (refresh || !text) {
            size_t n = 0;
            char *fresh = fetch(i ? project : mupen, cancel, error, &n);
            if (fresh &&
                (!n || memchr(fresh, 0, n) || (!strstr(fresh, "crc ") && (*fresh != '[' || !strstr(fresh, "Name=")))))
                SDL_strlcpy(error, "Invalid cheat download", sizeof(error));
            else if (fresh && !atomic_load(cancel)) {
                if (recomp_create_directories(dir) && recomp_save_image(path, (const uint8_t *)fresh, n)) {
                    free(text);
                    text = fresh;
                    fresh = NULL;
                    status = "downloaded";
                } else
                    SDL_strlcpy(error, "Cannot save cheat download cache", sizeof(error));
            }
            free(fresh);
            if (strcmp(status, "downloaded")) {
                if (text)
                    SDL_strlcat(error, "; using cache", sizeof(error));
                status = error;
            }
        }
        db->sources[i] = text;
        unsigned n = parse(db, text, key, name, i != 0, seen);
        size_t at = strlen(db->status);
        snprintf(db->status + at, sizeof(db->status) - at, "%s%s: %s (%u cheats)", at ? "\n" : "", name, status, n);
    }
    free(seen);
    if (!db->count)
        SDL_strlcat(db->status, "\nNo codes found for this ROM's CRC and region. Refresh to retry.",
                    sizeof(db->status));
}
