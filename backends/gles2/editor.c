#include "internal.h"
#include "editor_ai.h"
#include "texture_pack.h"
#include "host.h"
#include "runtime.h"
#include "stb_easy_font.h"
#include <SDL.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    thumb_side = 64,
    recent_limit = 128,
    bucket_count = 256,
    columns = 7,
    rows = 4,
    page_size = columns * rows,
    cell_size = 96,
    margin = 8,
    grid_height = rows * cell_size + margin,
    footer_height = 90
};
struct Gles2EditorPreview {
    unsigned width, height;
    uint8_t pixels[thumb_side * thumb_side * 4];
};
typedef struct Thumbnail {
    uint64_t key, frame;
    struct Thumbnail *newer, *older, *next_hash;
    unsigned width, height, gallery_index;
    bool replaced, ai, loose;
    uint8_t *original;
    Gles2EditorPreview preview;
} Thumbnail;
struct Gles2TextureEditor {
    char *directory;
    unsigned limit;
    Gles2AiJob *job;
    Thumbnail *job_texture;
    Thumbnail *buckets[bucket_count], *newest, *oldest;
    Thumbnail **items; /* Frozen in draw order while the gallery is open. */
    uint64_t frame;
    unsigned count, selected;
};
static void sample(Gles2EditorPreview *t, unsigned width, unsigned height, const uint8_t *rgba) {
    unsigned longest = SDL_max(width, height);
    t->width = SDL_max(1u, thumb_side * width / longest);
    t->height = SDL_max(1u, thumb_side * height / longest);
    for (unsigned y = 0; y < t->height; ++y)
        for (unsigned x = 0; x < t->width; ++x) {
            size_t offset = ((size_t)(y * height / t->height) * width + x * width / t->width) * 4;
            memcpy(t->pixels + ((size_t)y * t->width + x) * 4, rgba + offset, 4);
        }
}
Gles2EditorPreview *gles2_editor_make_preview(unsigned width, unsigned height, const uint8_t *rgba) {
    if (!width || !height || width > 4096 || height > 4096 || !rgba)
        return NULL;
    Gles2EditorPreview *preview = calloc(1, sizeof(*preview));
    if (preview)
        sample(preview, width, height, rgba);
    return preview;
}
static const char *install(const char *source, const char *directory, Thumbnail *t, unsigned limit) {
    SDL_RWops *file = SDL_RWFromFile(source, "rb");
    if (!file)
        return "Could not open PNG";
    Sint64 size = SDL_RWsize(file);
    if (size < 8 || size > GLES2_TEXTURE_MAX_PNG_BYTES) {
        SDL_RWclose(file);
        return "PNG must be 16 MiB or smaller";
    }
    size_t n = 0;
    void *bytes = SDL_LoadFile_RW(file, &n, 1);
    if (!bytes)
        return "Could not read PNG";
    Gles2PackImage image = {0};
    const char *error = NULL;
    char *target = NULL;
    if (!gles2_texture_decode_png(bytes, n, t->width, t->height, limit, &image))
        error = "Select a valid PNG with the original aspect ratio and size within the texture limit";
    else if (SDL_asprintf(&target, "%s/%016" PRIx64 ".gles.png", directory, t->key) < 0 ||
             !recomp_create_directories(directory) || !recomp_save_image(target, bytes, n))
        error = "Could not save replacement";
    else {
        sample(&t->preview, image.width, image.height, image.pixels);
        t->replaced = t->loose = true;
    }
    SDL_free(target);
    SDL_free(bytes);
    gles2_texture_pack_free_image(&image);
    return error;
}
static SDL_Rect cell(unsigned index) {
    return (SDL_Rect){margin + (int)(index % columns) * cell_size, margin + (int)(index / columns) * cell_size,
                      cell_size - margin, cell_size - margin};
}

static void color(SDL_Renderer *r, uint8_t red, uint8_t green, uint8_t blue) {
    SDL_SetRenderDrawColor(r, red, green, blue, 255);
}

static void draw_text(SDL_Renderer *renderer, int x, int y, const char *text) {
    char label[100];
    snprintf(label, sizeof(label), "%s", text);
    _Alignas(float) char vertices[32000];
    const int quads = stb_easy_font_print(0, 0, label, NULL, vertices, sizeof(vertices));
    for (int i = 0; i < quads; ++i) {
        float top_left[2], bottom_right[2];
        memcpy(top_left, vertices + i * 64, sizeof(top_left));
        memcpy(bottom_right, vertices + i * 64 + 32, sizeof(bottom_right));
        SDL_Rect rect = {x + (int)(top_left[0]), y + (int)(top_left[1]), (int)(bottom_right[0] - top_left[0]),
                         (int)(bottom_right[1] - top_left[1])};
        SDL_RenderFillRect(renderer, &rect);
    }
}
Gles2TextureEditor *gles2_editor_create(const char *dir) {
    if (!dir || !*dir)
        return NULL;
    Gles2TextureEditor *s = calloc(1, sizeof(*s));
    if (s && !(s->directory = SDL_strdup(dir))) {
        free(s);
        return NULL;
    }
    return s;
}
static unsigned bucket(uint64_t key) {
    return (unsigned)(key ^ (key >> 32)) % bucket_count;
}

static Thumbnail *find(const Gles2TextureEditor *s, uint64_t key) {
    for (Thumbnail *t = s->buckets[bucket(key)]; t; t = t->next_hash)
        if (t->key == key)
            return t;
    return NULL;
}

static void forget_oldest(Gles2TextureEditor *s) {
    Thumbnail *t = s->oldest;
    Thumbnail **link = &s->buckets[bucket(t->key)];
    while (*link != t)
        link = &(*link)->next_hash;
    *link = t->next_hash;
    s->oldest = t->newer;
    if (s->oldest)
        s->oldest->older = NULL;
    else
        s->newest = NULL;
    free(t->original);
    free(t);
    --s->count;
}

void gles2_editor_destroy(Gles2TextureEditor *s) {
    if (s) {
        while (s->oldest)
            forget_oldest(s);
        SDL_free(s->directory);
        free(s);
    }
}

/* The cache and gallery have independent lifetimes. Track actual tile use even
 * when its GPU upload is shared or still resident. Move links, not pixel buffers. */
static void mark_used(Gles2TextureEditor *s, Thumbnail *t) {
    t->frame = s->frame;
    if (s->newest == t)
        return;
    if (t->newer)
        t->newer->older = t->older;
    if (t->older)
        t->older->newer = t->newer;
    if (s->oldest == t)
        s->oldest = t->newer;
    t->older = s->newest;
    t->newer = NULL;
    if (s->newest)
        s->newest->newer = t;
    else
        s->oldest = t;
    s->newest = t;
}

bool gles2_editor_touch(Gles2TextureEditor *s, uint64_t key, bool replaced) {
    Thumbnail *t = find(s, key);
    if (!t || t->replaced != replaced || !t->preview.width)
        return false;
    mark_used(s, t);
    return true;
}

void gles2_editor_presented(Gles2TextureEditor *s) {
    if (!s || !s->newest || s->newest->frame != s->frame)
        return;
    /* VI can still display the preceding draw. Keep both textured frames in
     * full, then fill the recent history from older draws. Idle VI ticks do not age it. */
    while (s->count > recent_limit && s->frame - s->oldest->frame > 1)
        forget_oldest(s);
    ++s->frame;
}

static Thumbnail *observe(Gles2TextureEditor *s, uint64_t key, unsigned width, unsigned height, bool replaced) {
    if (!width || !height || width > 4096 || height > 4096)
        return NULL;
    Thumbnail *t = find(s, key);
    if (!t) {
        t = calloc(1, sizeof(*t));
        if (!t)
            return NULL;
        unsigned slot = bucket(key);
        t->key = key;
        t->width = width;
        t->height = height;
        t->next_hash = s->buckets[slot];
        s->buckets[slot] = t;
        ++s->count;
    }
    t->replaced = replaced;
    mark_used(s, t);
    return t;
}
/* Keep native texels independently of the displayed preview. Repeated AI edits
 * always start from the same tile, including when a pack was loaded at startup. */
static uint8_t *allocate_original(Thumbnail *t) {
    if (!t || t->original || t->width > GLES2_TEXTURE_MAX_SIDE / 4 || t->height > GLES2_TEXTURE_MAX_SIDE / 4)
        return NULL;
    t->original = malloc((size_t)t->width * t->height * 4);
    return t->original;
}

void gles2_editor_observe(Gles2TextureEditor *editor, uint64_t key, unsigned width, unsigned height,
                          const uint8_t *rgba) {
    if (!editor || !rgba || gles2_editor_touch(editor, key, false))
        return;
    Thumbnail *t = observe(editor, key, width, height, false);
    if (t) {
        uint8_t *original = allocate_original(t);
        if (original)
            memcpy(original, rgba, (size_t)width * height * 4);
        sample(&t->preview, width, height, rgba);
    }
}
uint8_t *gles2_editor_observe_replacement(Gles2TextureEditor *editor, uint64_t key, unsigned width, unsigned height,
                                          const Gles2EditorPreview *preview) {
    if (!editor || !preview)
        return NULL;
    Thumbnail *t = observe(editor, key, width, height, true);
    if (t)
        t->preview = *preview;
    return allocate_original(t);
}
static const char *remove_replacement(Thumbnail *t, Gles2TextureEditor *editor, const char *zip,
                                      Gles2TexturePack *pack) {
    const char *error = t->ai ? gles2_ai_remove(zip, t->key) : gles2_editor_remove_loose(editor->directory, t->key);
    if (error)
        return error;
    if (t->ai)
        t->ai = false;
    else
        t->loose = false;
    t->replaced = t->loose;
    Gles2PackImage image;
    size_t source;
    Gles2PackTile tile = {t->key, t->width, t->height};
    if (t->loose && pack && gles2_texture_pack_local(pack, t->key, &source) &&
        gles2_texture_pack_decode(pack, source, tile, editor->limit, &image)) {
        sample(&t->preview, image.width, image.height, image.pixels);
        gles2_texture_pack_free_image(&image);
    } else if (t->original)
        sample(&t->preview, t->width, t->height, t->original);
    else
        t->preview.width = 0;
    return NULL;
}

static const SDL_Rect improve_button = {margin, grid_height + 34, 124, 22};
static const SDL_Rect remove_button = {margin + 132, grid_height + 34, 144, 22};

static bool can_improve(const Gles2TextureEditor *s, const Thumbnail *t) {
    return !s->job && t && t->original && t->width <= s->limit / GLES2_AI_SCALE &&
           t->height <= s->limit / GLES2_AI_SCALE;
}

static void button(SDL_Renderer *r, const SDL_Rect *rect, const char *label, bool enabled) {
    color(r, enabled ? 51 : 39, enabled ? 84 : 42, enabled ? 99 : 46);
    SDL_RenderFillRect(r, rect);
    uint8_t ink = enabled ? 240 : 130;
    color(r, ink, ink, ink);
    draw_text(r, rect->x + 8, rect->y + 6, label);
}

static const char guidance[] = "Esc or F8 to return to the game";

static void paint(SDL_Renderer *renderer, Gles2TextureEditor *state, const char *message) {
    color(renderer, 25, 27, 31);
    SDL_RenderClear(renderer);
    const unsigned page = (state->selected / page_size) * page_size;
    for (unsigned i = page; i < SDL_min(page + page_size, state->count); i++) {
        const Thumbnail *t = state->items[i];
        SDL_Rect bounds = cell(i - page);
        color(renderer, i == state->selected ? 110 : 57, i == state->selected ? 228 : (t->replaced ? 136 : 60),
              i == state->selected ? 255 : 67);
        SDL_RenderFillRect(renderer, &bounds);
        if (!t->preview.width)
            continue;
        const SDL_Rect preview = {bounds.x + (bounds.w - (int)(t->preview.width)) / 2,
                                  bounds.y + (bounds.h - (int)(t->preview.height)) / 2, (int)(t->preview.width),
                                  (int)(t->preview.height)};
        const int checker = 8;
        for (int y = 0; y < preview.h; y += checker)
            for (int x = 0; x < preview.w; x += checker) {
                uint8_t shade = ((x / checker + y / checker) & 1) ? 100 : 64;
                color(renderer, shade, shade, shade);
                SDL_Rect square = {preview.x + x, preview.y + y, SDL_min(checker, preview.w - x),
                                   SDL_min(checker, preview.h - y)};
                SDL_RenderFillRect(renderer, &square);
            }
        SDL_Texture *image =
            SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, preview.w, preview.h);
        if (image) {
            SDL_SetTextureBlendMode(image, SDL_BLENDMODE_BLEND);
            if (SDL_UpdateTexture(image, NULL, t->preview.pixels, preview.w * 4) == 0)
                SDL_RenderCopy(renderer, image, NULL, &preview);
            SDL_DestroyTexture(image);
        }
    }
    color(renderer, 211, 222, 233);
    char label[100];
    if (!state->count) {
        snprintf(label, sizeof(label), "No editable textures seen yet");
    } else {
        const Thumbnail *t = state->items[state->selected];
        snprintf(label, sizeof(label), "%016" PRIx64 " / native %u x %u / %u of %u%s", t->key, t->width, t->height,
                 state->selected + 1, state->count, t->replaced ? " / replacement" : "");
    }
    draw_text(renderer, margin, grid_height + 4, label);
    draw_text(renderer, margin, grid_height + 20,
              "Click a scene triangle or thumbnail. Shift-click for the second tile. Drop a PNG to replace.");
    color(renderer, 110, 228, 255);
    Thumbnail *t = state->count ? state->items[state->selected] : NULL;
    button(renderer, &improve_button, "Improve with AI", can_improve(state, t));
    button(renderer, &remove_button, t && t->ai ? "Remove AI version" : "Remove replacement",
           !state->job && t && (t->ai || t->loose));
    color(renderer, 110, 228, 255);
    if (state->job)
        snprintf(label, sizeof(label), "Improving %016" PRIx64 " with Codex... Esc cancels", state->job_texture->key);
    else if (t && !can_improve(state, t) && message == guidance)
        snprintf(label, sizeof(label), "Native source unavailable or output exceeds the %u px texture limit",
                 state->limit);
    else
        snprintf(label, sizeof(label), "%s", message);
    draw_text(renderer, margin, grid_height + 65, label);
    SDL_RenderPresent(renderer);
}

static void select_delta(Gles2TextureEditor *state, int delta) {
    if (state->count)
        state->selected = (unsigned)(SDL_clamp((int)(state->selected) + delta, 0, (int)(state->count) - 1));
}

Gles2EditorResult gles2_editor_run(RecompRDPGles2 *gpu) {
    Gles2TextureEditor *editor = gpu->editor;
    SDL_Window *game = gpu->window;
    const char *zip = gpu->texture_zip;
    Gles2TexturePack *pack = gpu->texture_pack;
    unsigned max_texture_size = gpu->max_texture_size;
    if (!zip || !editor || !game)
        return GLES2_EDITOR_OPEN_FAILED;
    const Uint32 game_window = SDL_GetWindowID(game);
    int x, y, w;
    SDL_GetWindowPosition(game, &x, &y);
    SDL_GetWindowSize(game, &w, NULL);
    const int gallery_w = columns * cell_size + margin, gallery_h = grid_height + footer_height;
    SDL_Rect desktop;
    if (SDL_GetDisplayUsableBounds(SDL_GetWindowDisplayIndex(game), &desktop) == 0) {
        int right = desktop.x + desktop.w - gallery_w;
        x = x + w <= right || x - gallery_w < desktop.x ? SDL_min(x + w, right) : x - gallery_w;
        x = SDL_max(x, desktop.x);
        y = SDL_clamp(y, desktop.y, SDL_max(desktop.y, desktop.y + desktop.h - gallery_h));
    }
    SDL_Window *window = SDL_CreateWindow("GLES2 Texture Editor", x, y, gallery_w, gallery_h, SDL_WINDOW_ALWAYS_ON_TOP);
    if (!window) {
        SDL_Log("GLES2 texture editor: %s", SDL_GetError());
        return GLES2_EDITOR_OPEN_FAILED;
    }
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        SDL_DestroyWindow(window);
        SDL_Log("GLES2 texture editor: %s", SDL_GetError());
        return GLES2_EDITOR_OPEN_FAILED;
    }
    if (editor->count) {
        editor->items = calloc(editor->count, sizeof(*editor->items));
        if (!editor->items) {
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            return GLES2_EDITOR_OPEN_FAILED;
        }
        unsigned index = 0;
        for (Thumbnail *t = editor->newest; t; t = t->older)
            editor->items[index++] = t;
    }
    const Uint32 id = SDL_GetWindowID(window);
    const Uint8 drop_state = SDL_EventState(SDL_DROPFILE, SDL_QUERY);
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    SDL_bool click_through = SDL_GetHintBoolean(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, SDL_FALSE);
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    editor->limit = SDL_min((unsigned)GLES2_TEXTURE_MAX_SIDE, max_texture_size);
    editor->selected = 0;
    for (unsigned i = 0; i < editor->count; ++i) {
        Thumbnail *t = editor->items[i];
        t->gallery_index = i;
        size_t source;
        t->ai = gles2_texture_pack_ai(pack, t->key, &source);
        t->loose = pack && gles2_texture_pack_local(pack, t->key, &source);
    }
    const char *message = guidance;
    bool changed = false, quit = false, closed = false;
    unsigned hovered = 0;
    recomp_gles2_editor_display(gpu, true, hovered);
    paint(renderer, editor, message);
    while (!closed) {
        SDL_Event event;
        if (editor->job) {
            Thumbnail *t = editor->job_texture;
            Gles2PackImage image = {0};
            const char *error;
            if (gles2_ai_poll(editor->job, zip, t->key, &image, &error)) {
                if (!error) {
                    sample(&t->preview, image.width, image.height, image.pixels);
                    t->ai = t->replaced = changed = true;
                }
                gles2_texture_pack_free_image(&image);
                gles2_ai_destroy(editor->job);
                editor->job = NULL;
                message = error ? error : "Replacement saved to ZIP";
                SDL_Log("GLES2 texture editor: %s", message);
                paint(renderer, editor, message);
            }
        }
        if (editor->job) {
            if (!SDL_WaitEventTimeout(&event, 100))
                continue;
        } else if (!SDL_WaitEvent(&event))
            break;
        if (event.type == SDL_QUIT || (event.type == SDL_WINDOWEVENT && event.window.windowID == game_window &&
                                       event.window.event == SDL_WINDOWEVENT_CLOSE)) {
            quit = closed = true;
        } else if (event.type == SDL_MOUSEMOTION && event.motion.windowID == game_window) {
            unsigned hit = recomp_gles2_editor_hit(gpu, event.motion.x, event.motion.y);
            if (hovered != hit) {
                hovered = hit;
                recomp_gles2_editor_display(gpu, true, hovered);
            }
        } else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.windowID == game_window &&
                   event.button.button == SDL_BUTTON_LEFT) {
            hovered = recomp_gles2_editor_hit(gpu, event.button.x, event.button.y);
            uint64_t key;
            bool secondary = (SDL_GetModState() & KMOD_SHIFT) != 0;
            if (recomp_gles2_pick_key(gpu, hovered, secondary, &key)) {
                Thumbnail *t = find(editor, key);
                if (t)
                    editor->selected = t->gallery_index;
                message = t ? "Selected the triangle's texture" : "This texture is no longer in the gallery";
            } else
                message = hovered ? "This triangle has no texture" : "No triangle at this point";
            recomp_gles2_editor_display(gpu, true, hovered);
            paint(renderer, editor, message);
        } else if (event.type == SDL_WINDOWEVENT && event.window.windowID == game_window &&
                   (event.window.event == SDL_WINDOWEVENT_EXPOSED ||
                    event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                    event.window.event == SDL_WINDOWEVENT_LEAVE)) {
            hovered = 0;
            recomp_gles2_editor_display(gpu, true, hovered);
        } else if (event.type == SDL_KEYDOWN && event.key.windowID == game_window && !event.key.repeat &&
                   (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE || event.key.keysym.scancode == SDL_SCANCODE_F8)) {
            closed = true;
        } else if (event.type == SDL_WINDOWEVENT && event.window.windowID == id) {
            if (event.window.event == SDL_WINDOWEVENT_CLOSE)
                closed = true;
            else if (event.window.event == SDL_WINDOWEVENT_EXPOSED)
                paint(renderer, editor, message);
        } else if (event.type == SDL_KEYDOWN && event.key.windowID == id) {
            if (event.key.repeat &&
                (event.key.keysym.scancode == SDL_SCANCODE_ESCAPE || event.key.keysym.scancode == SDL_SCANCODE_F8))
                continue;
            switch (event.key.keysym.scancode) {
                case SDL_SCANCODE_ESCAPE:
                case SDL_SCANCODE_F8:
                    closed = true;
                    break;
                case SDL_SCANCODE_LEFT:
                    select_delta(editor, -1);
                    break;
                case SDL_SCANCODE_RIGHT:
                    select_delta(editor, 1);
                    break;
                case SDL_SCANCODE_UP:
                    select_delta(editor, -(int)(columns));
                    break;
                case SDL_SCANCODE_DOWN:
                    select_delta(editor, columns);
                    break;
                case SDL_SCANCODE_PAGEUP:
                    select_delta(editor, -(int)(page_size));
                    break;
                case SDL_SCANCODE_PAGEDOWN:
                    select_delta(editor, page_size);
                    break;
                default:
                    continue;
            }
            if (!closed) {
                message = guidance;
                paint(renderer, editor, message);
            }
        } else if (event.type == SDL_MOUSEWHEEL && event.wheel.windowID == id && event.wheel.y) {
            bool up = (event.wheel.y > 0) != (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED);
            select_delta(editor, up ? -(int)(page_size) : (int)(page_size));
            message = guidance;
            paint(renderer, editor, message);
        } else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.windowID == id &&
                   event.button.button == SDL_BUTTON_LEFT) {
            unsigned page = (editor->selected / page_size) * page_size;
            SDL_Point point = {event.button.x, event.button.y};
            Thumbnail *t = editor->count ? editor->items[editor->selected] : NULL;
            if (SDL_PointInRect(&point, &improve_button) && can_improve(editor, t)) {
                editor->job_texture = t;
                editor->job = gles2_ai_start(t->original, t->width, t->height, &message);
                paint(renderer, editor, message);
            } else if (SDL_PointInRect(&point, &remove_button) && event.button.clicks <= 1 && !editor->job && t &&
                       (t->ai || t->loose)) {
                const char *error = remove_replacement(t, editor, zip, pack);
                changed |= !error;
                message = error ? error : "Removed. Gameplay will reload the underlying texture";
                paint(renderer, editor, message);
            }
            for (unsigned i = page; i < SDL_min(page + page_size, editor->count); i++) {
                SDL_Rect bounds = cell(i - page);
                if (SDL_PointInRect(&point, &bounds)) {
                    editor->selected = i;
                    message = guidance;
                    paint(renderer, editor, message);
                    break;
                }
            }
        } else if (event.type == SDL_DROPFILE || event.type == SDL_DROPTEXT) {
            char *path = event.drop.file;
            if (event.type == SDL_DROPFILE && event.drop.windowID == id && editor->count && path) {
                Thumbnail *t = editor->items[editor->selected];
                const char *error = editor->job && editor->job_texture == t
                                        ? "Wait for this texture to finish before importing"
                                    : t->ai ? "Remove the AI version before importing a PNG"
                                            : install(path, editor->directory, t, max_texture_size);
                changed |= !error;
                message = error ? error : "Saved! Replacement applies when gameplay resumes";
                SDL_Log("GLES2 texture editor: %s", message);
                paint(renderer, editor, message);
            }
            SDL_free(path);
        }
    }
    gles2_ai_destroy(editor->job);
    editor->job = NULL;
    free(editor->items);
    editor->items = NULL;
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, click_through ? "1" : "0");
    SDL_EventState(SDL_DROPFILE, drop_state);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    if (!quit) {
        recomp_gles2_editor_display(gpu, false, 0);
        SDL_RaiseWindow(game);
    }
    return (Gles2EditorResult)((changed ? GLES2_EDITOR_CHANGED : 0) | (quit ? GLES2_EDITOR_QUIT : 0));
}
