#include "editor.h"
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
    history_size = 128,
    columns = 7,
    rows = 4,
    page_size = columns * rows,
    cell_size = 96,
    margin = 8,
    grid_height = rows * cell_size + margin,
    footer_height = 63
};
typedef struct Thumbnail {
    uint64_t key;
    unsigned width, height, preview_width, preview_height;
    bool replaced;
    uint8_t pixels[thumb_side * thumb_side * 4];
} Thumbnail;
struct Gles2TextureEditor {
    char *directory;
    Thumbnail history[history_size];
    unsigned count, next, selected;
};
static Thumbnail *recent(Gles2TextureEditor *s, unsigned i) {
    return &s->history[(s->next + history_size - 1 - i) % history_size];
}
static void sample(Thumbnail *t, unsigned width, unsigned height, const uint8_t *rgba) {
    unsigned longest = SDL_max(width, height);
    t->preview_width = SDL_max(1u, thumb_side * width / longest);
    t->preview_height = SDL_max(1u, thumb_side * height / longest);
    for (unsigned y = 0; y < t->preview_height; ++y)
        for (unsigned x = 0; x < t->preview_width; ++x) {
            size_t offset = ((size_t)(y * height / t->preview_height) * width + x * width / t->preview_width) * 4;
            memcpy(t->pixels + ((size_t)y * t->preview_width + x) * 4, rgba + offset, 4);
        }
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
        sample(t, image.width, image.height, image.pixels);
        t->replaced = true;
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
void gles2_editor_destroy(Gles2TextureEditor *s) {
    if (s) {
        SDL_free(s->directory);
        free(s);
    }
}

static void observe(Gles2TextureEditor *editor, uint64_t key, unsigned width, unsigned height, unsigned preview_width,
                    unsigned preview_height, const uint8_t *rgba, bool replacement) {
    if (!editor || !rgba || !width || !height || !preview_width || !preview_height || width > 4096 || height > 4096 ||
        preview_width > 4096 || preview_height > 4096)
        return;
    for (unsigned i = 0; i < editor->count; i++) {
        Thumbnail *old = recent(editor, i);
        if (old->key == key) {
            if (replacement != old->replaced) {
                old->replaced = replacement;
                sample(old, preview_width, preview_height, rgba);
            }
            return;
        }
    }
    Thumbnail *t = &editor->history[editor->next];
    t->key = key;
    t->width = width;
    t->height = height;
    t->replaced = replacement;
    sample(t, preview_width, preview_height, rgba);
    editor->next = (editor->next + 1) % history_size;
    editor->count = SDL_min(editor->count + 1, history_size);
}

void gles2_editor_observe(Gles2TextureEditor *editor, uint64_t key, unsigned width, unsigned height,
                          const uint8_t *rgba) {
    observe(editor, key, width, height, width, height, rgba, false);
}

void gles2_editor_observe_replacement(Gles2TextureEditor *editor, uint64_t key, unsigned width, unsigned height,
                                      unsigned preview_width, unsigned preview_height, const uint8_t *rgba) {
    observe(editor, key, width, height, preview_width, preview_height, rgba, true);
}

static void paint(SDL_Renderer *renderer, Gles2TextureEditor *state, const char *message) {
    color(renderer, 25, 27, 31);
    SDL_RenderClear(renderer);
    const unsigned page = (state->selected / page_size) * page_size;
    for (unsigned i = page; i < SDL_min(page + page_size, state->count); i++) {
        const Thumbnail *t = recent(state, i);
        SDL_Rect bounds = cell(i - page);
        color(renderer, i == state->selected ? 110 : 57, i == state->selected ? 228 : (t->replaced ? 136 : 60),
              i == state->selected ? 255 : 67);
        SDL_RenderFillRect(renderer, &bounds);
        const SDL_Rect preview = {bounds.x + (bounds.w - (int)(t->preview_width)) / 2,
                                  bounds.y + (bounds.h - (int)(t->preview_height)) / 2, (int)(t->preview_width),
                                  (int)(t->preview_height)};
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
            if (SDL_UpdateTexture(image, NULL, t->pixels, preview.w * 4) == 0)
                SDL_RenderCopy(renderer, image, NULL, &preview);
            SDL_DestroyTexture(image);
        }
    }
    color(renderer, 211, 222, 233);
    char label[100];
    if (!state->count) {
        snprintf(label, sizeof(label), "No editable textures seen yet");
    } else {
        const Thumbnail *t = recent(state, state->selected);
        snprintf(label, sizeof(label), "%016" PRIx64 " / native %u x %u / %u of %u%s", t->key, t->width, t->height,
                 state->selected + 1, state->count, t->replaced ? " / replacement" : "");
    }
    draw_text(renderer, margin, grid_height + 4, label);
    draw_text(renderer, margin, grid_height + 20,
              "Select a thumbnail and drop a PNG. Browse with mouse wheel or arrows.");
    color(renderer, 110, 228, 255);
    draw_text(renderer, margin, grid_height + 37, message);
    SDL_RenderPresent(renderer);
}

static void select_delta(Gles2TextureEditor *state, int delta) {
    if (state->count)
        state->selected = (unsigned)(SDL_clamp((int)(state->selected) + delta, 0, (int)(state->count) - 1));
}

static const char guidance[] = "Esc or F8 to return to the game";

Gles2EditorResult gles2_editor_run(Gles2TextureEditor *editor, SDL_Window *game, unsigned max_texture_size) {
    if (!editor || !game)
        return GLES2_EDITOR_OPEN_FAILED;
    const Uint32 game_window = SDL_GetWindowID(game);
    SDL_Window *window = SDL_CreateWindow("GLES2 Texture Editor", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                          columns * cell_size + margin, grid_height + footer_height, 0);
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
    const Uint32 id = SDL_GetWindowID(window);
    const Uint8 drop_state = SDL_EventState(SDL_DROPFILE, SDL_QUERY);
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    editor->selected = 0;
    const char *message = guidance;
    bool changed = false, quit = false, closed = false;
    paint(renderer, editor, message);
    while (!closed) {
        SDL_Event event;
        if (!SDL_WaitEvent(&event))
            break;
        if (event.type == SDL_QUIT || (event.type == SDL_WINDOWEVENT && event.window.windowID == game_window &&
                                       event.window.event == SDL_WINDOWEVENT_CLOSE)) {
            quit = closed = true;
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
                const char *error =
                    install(path, editor->directory, recent(editor, editor->selected), max_texture_size);
                changed |= !error;
                message = error ? error : "Saved! Replacement applies when gameplay resumes";
                SDL_Log("GLES2 texture editor: %s", message);
                paint(renderer, editor, message);
            }
            SDL_free(path);
        }
    }
    SDL_EventState(SDL_DROPFILE, drop_state);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    if (!quit)
        SDL_RaiseWindow(game);
    return (Gles2EditorResult)((changed ? GLES2_EDITOR_CHANGED : 0) | (quit ? GLES2_EDITOR_QUIT : 0));
}
