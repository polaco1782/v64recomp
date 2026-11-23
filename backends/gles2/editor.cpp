#include "editor.h"
#include "texture_pack.h"
#include "stb/stb_easy_font.h"
#include <SDL.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <inttypes.h>
#include <memory>
#include <new>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;
namespace {
constexpr unsigned thumb_side = 64;
constexpr unsigned history_size = 128;
constexpr unsigned columns = 7;
constexpr unsigned rows = 4;
constexpr unsigned page_size = columns * rows;
constexpr int cell_size = 96;
constexpr int margin = 8;
constexpr int grid_height = rows * cell_size + margin;
constexpr int footer_height = 63;

struct Thumbnail {
    uint64_t key = 0;
    unsigned width = 0, height = 0;
    unsigned preview_width = 0, preview_height = 0;
    bool replaced = false;
    std::array<uint8_t, thumb_side * thumb_side * 4> pixels;
};

void sample(Thumbnail& t, unsigned width, unsigned height, const uint8_t *rgba) {
    t.preview_width = std::max(1u, thumb_side * width / std::max(width, height));
    t.preview_height = std::max(1u, thumb_side * height / std::max(width, height));
    for (unsigned y = 0; y < t.preview_height; y++) {
        for (unsigned x = 0; x < t.preview_width; x++) {
            const auto offset = (size_t(y * height / t.preview_height) * width + x * width / t.preview_width) * 4;
            std::copy_n(rgba + offset, 4, t.pixels.data() + (size_t(y) * t.preview_width + x) * 4);
        }
    }
}

bool store_png(const fs::path& directory, uint64_t key, const std::vector<uint8_t>& png) {
    char name[32];
    std::snprintf(name, sizeof(name), "%016" PRIx64 ".v5.png", key);
    std::error_code error;
    fs::create_directories(directory, error);
    if (error)
        return false;
    /* Reserve our own staging directory, then rename on the same filesystem.
     * Never truncate an existing override or rewrite the user's source ZIP. */
    fs::path staging;
    bool reserved = false;
    for (unsigned n = 0; n < 8 && !reserved; n++) {
        char temporary[64];
        std::snprintf(temporary, sizeof(temporary), ".v64-texture-%016" PRIx64 "-%08x-%u", key, SDL_GetTicks(), n);
        staging = directory / temporary;
        reserved = fs::create_directory(staging, error);
        if (error)
            return false;
    }
    if (!reserved)
        return false;
    auto source = staging / name;
    auto target = directory / name;
    bool ok = false;
    {
        std::ofstream file(source, std::ios::binary);
        file.write(reinterpret_cast<const char *>(png.data()), std::streamsize(png.size()));
        file.close();
        if (file) {
#ifdef _WIN32
            ok = MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
            fs::rename(source, target, error);
            ok = !error;
#endif
        }
    }
    fs::remove(source, error);
    fs::remove(staging, error);
    return ok;
}

const char *install(const char *path, const fs::path& directory, Thumbnail& t, unsigned max_size) {
    std::error_code error;
    const fs::path source = fs::u8path(path);
    if (!fs::is_regular_file(source, error))
        return "Drop a local PNG file";
    const auto bytes = fs::file_size(source, error);
    if (error || bytes > GLES2_TEXTURE_MAX_PNG_BYTES || bytes < 8)
        return "PNG must be 16 MiB or smaller";
    std::vector<uint8_t> png(static_cast<size_t>(bytes));
    std::ifstream file(source, std::ios::binary);
    if (!file.read(reinterpret_cast<char *>(png.data()), std::streamsize(png.size())))
        return "Could not read PNG";
    Gles2PackImage image{};
    const auto release = [](Gles2PackImage *p) { gles2_texture_pack_free_image(p); };
    std::unique_ptr<Gles2PackImage, decltype(release)> cleanup(&image, release);
    if (!gles2_texture_decode_png(png.data(), png.size(), t.width, t.height, max_size, &image))
        return "Select a valid PNG with the original aspect ratio and size within the texture limit";
    if (!store_png(directory, t.key, png))
        return "Could not save replacement";
    sample(t, image.width, image.height, image.pixels);
    t.replaced = true;
    return nullptr;
}

SDL_Rect cell(unsigned index) {
    return {margin + int(index % columns) * cell_size, margin + int(index / columns) * cell_size, cell_size - margin,
            cell_size - margin};
}

void color(SDL_Renderer *r, uint8_t red, uint8_t green, uint8_t blue) {
    SDL_SetRenderDrawColor(r, red, green, blue, 255);
}

void draw_text(SDL_Renderer *renderer, int x, int y, const char *text) {
    char label[100];
    std::snprintf(label, sizeof(label), "%s", text);
    alignas(float) char vertices[32000];
    const int quads = stb_easy_font_print(0, 0, label, nullptr, vertices, sizeof(vertices));
    for (int i = 0; i < quads; ++i) {
        float top_left[2], bottom_right[2];
        std::memcpy(top_left, vertices + i * 64, sizeof(top_left));
        std::memcpy(bottom_right, vertices + i * 64 + 32, sizeof(bottom_right));
        SDL_Rect rect{x + int(top_left[0]), y + int(top_left[1]), int(bottom_right[0] - top_left[0]),
                      int(bottom_right[1] - top_left[1])};
        SDL_RenderFillRect(renderer, &rect);
    }
}
}

struct Gles2TextureEditor {
    explicit Gles2TextureEditor(const fs::path& path) : directory(path) {}
    fs::path directory;
    std::array<Thumbnail, history_size> history;
    unsigned count = 0, next = 0, selected = 0;

    Thumbnail& recent(unsigned index) {
        return history[(next + history_size - 1 - index) % history_size];
    }
};

Gles2TextureEditor *gles2_editor_create(const char *overrides_dir) {
    try {
        return overrides_dir && *overrides_dir ? new Gles2TextureEditor(fs::u8path(overrides_dir)) : nullptr;
    } catch (const std::exception&) {
        return nullptr;
    }
}

void gles2_editor_destroy(Gles2TextureEditor *editor) {
    delete editor;
}

static void observe(Gles2TextureEditor *editor, uint64_t key, unsigned width, unsigned height, unsigned preview_width,
                    unsigned preview_height, const uint8_t *rgba, bool replacement) {
    if (!editor || !rgba || !width || !height || !preview_width || !preview_height || width > 4096 || height > 4096 ||
        preview_width > 4096 || preview_height > 4096)
        return;
    for (unsigned i = 0; i < editor->count; i++) {
        auto& old = editor->recent(i);
        if (old.key == key) {
            if (replacement != old.replaced) {
                old.replaced = replacement;
                sample(old, preview_width, preview_height, rgba);
            }
            return;
        }
    }
    auto& t = editor->history[editor->next];
    t.key = key;
    t.width = width;
    t.height = height;
    t.replaced = replacement;
    sample(t, preview_width, preview_height, rgba);
    editor->next = (editor->next + 1) % history_size;
    editor->count = std::min(editor->count + 1, history_size);
}

void gles2_editor_observe(Gles2TextureEditor *editor, uint64_t key, unsigned width, unsigned height,
                          const uint8_t *rgba) {
    observe(editor, key, width, height, width, height, rgba, false);
}

void gles2_editor_observe_replacement(Gles2TextureEditor *editor, uint64_t key, unsigned width, unsigned height,
                                      unsigned preview_width, unsigned preview_height, const uint8_t *rgba) {
    observe(editor, key, width, height, preview_width, preview_height, rgba, true);
}

namespace {
void paint(SDL_Renderer *renderer, Gles2TextureEditor& state, const char *message) {
    color(renderer, 25, 27, 31);
    SDL_RenderClear(renderer);
    const unsigned page = (state.selected / page_size) * page_size;
    for (unsigned i = page; i < std::min(page + page_size, state.count); i++) {
        const auto& t = state.recent(i);
        SDL_Rect bounds = cell(i - page);
        color(renderer, i == state.selected ? 110 : 57, i == state.selected ? 228 : (t.replaced ? 136 : 60),
              i == state.selected ? 255 : 67);
        SDL_RenderFillRect(renderer, &bounds);
        const SDL_Rect preview{bounds.x + (bounds.w - int(t.preview_width)) / 2,
                               bounds.y + (bounds.h - int(t.preview_height)) / 2, int(t.preview_width),
                               int(t.preview_height)};
        constexpr int checker = 8;
        for (int y = 0; y < preview.h; y += checker)
            for (int x = 0; x < preview.w; x += checker) {
                uint8_t shade = ((x / checker + y / checker) & 1) ? 100 : 64;
                color(renderer, shade, shade, shade);
                SDL_Rect square{preview.x + x, preview.y + y, std::min(checker, preview.w - x),
                                std::min(checker, preview.h - y)};
                SDL_RenderFillRect(renderer, &square);
            }
        SDL_Texture *image =
            SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, preview.w, preview.h);
        if (image) {
            SDL_SetTextureBlendMode(image, SDL_BLENDMODE_BLEND);
            if (SDL_UpdateTexture(image, nullptr, t.pixels.data(), preview.w * 4) == 0)
                SDL_RenderCopy(renderer, image, nullptr, &preview);
            SDL_DestroyTexture(image);
        }
    }
    color(renderer, 211, 222, 233);
    char label[100];
    if (!state.count) {
        std::snprintf(label, sizeof(label), "No editable textures seen yet");
    } else {
        const auto& t = state.recent(state.selected);
        std::snprintf(label, sizeof(label), "%016" PRIx64 " / native %u x %u / %u of %u%s", t.key, t.width, t.height,
                      state.selected + 1, state.count, t.replaced ? " / replacement" : "");
    }
    draw_text(renderer, margin, grid_height + 4, label);
    draw_text(renderer, margin, grid_height + 20,
              "Select a thumbnail and drop a PNG. Browse with mouse wheel or arrows.");
    color(renderer, 110, 228, 255);
    draw_text(renderer, margin, grid_height + 37, message);
    SDL_RenderPresent(renderer);
}

void select_delta(Gles2TextureEditor& state, int delta) {
    if (state.count)
        state.selected = unsigned(std::clamp(int(state.selected) + delta, 0, int(state.count) - 1));
}

constexpr char guidance[] = "Esc or F8 to return to the game";
}

Gles2EditorResult gles2_editor_run(Gles2TextureEditor *editor, SDL_Window *game, unsigned max_texture_size) {
    if (!editor || !game)
        return GLES2_EDITOR_OPEN_FAILED;
    const Uint32 game_window = SDL_GetWindowID(game);
    using WindowPtr = std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)>;
    using RendererPtr = std::unique_ptr<SDL_Renderer, decltype(&SDL_DestroyRenderer)>;
    WindowPtr window(SDL_CreateWindow("GLES2 Texture Editor", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                      columns * cell_size + margin, grid_height + footer_height, 0),
                     SDL_DestroyWindow);
    if (!window) {
        SDL_Log("GLES2 texture editor: %s", SDL_GetError());
        return GLES2_EDITOR_OPEN_FAILED;
    }
    RendererPtr renderer(SDL_CreateRenderer(window.get(), -1, SDL_RENDERER_SOFTWARE), SDL_DestroyRenderer);
    if (!renderer) {
        SDL_Log("GLES2 texture editor: %s", SDL_GetError());
        return GLES2_EDITOR_OPEN_FAILED;
    }
    const Uint32 id = SDL_GetWindowID(window.get());
    const Uint8 drop_state = SDL_EventState(SDL_DROPFILE, SDL_QUERY);
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);
    editor->selected = 0;
    const char *message = guidance;
    bool changed = false, quit = false, closed = false;
    paint(renderer.get(), *editor, message);
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
                paint(renderer.get(), *editor, message);
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
                    select_delta(*editor, -1);
                    break;
                case SDL_SCANCODE_RIGHT:
                    select_delta(*editor, 1);
                    break;
                case SDL_SCANCODE_UP:
                    select_delta(*editor, -int(columns));
                    break;
                case SDL_SCANCODE_DOWN:
                    select_delta(*editor, columns);
                    break;
                case SDL_SCANCODE_PAGEUP:
                    select_delta(*editor, -int(page_size));
                    break;
                case SDL_SCANCODE_PAGEDOWN:
                    select_delta(*editor, page_size);
                    break;
                default:
                    continue;
            }
            if (!closed) {
                message = guidance;
                paint(renderer.get(), *editor, message);
            }
        } else if (event.type == SDL_MOUSEWHEEL && event.wheel.windowID == id && event.wheel.y) {
            bool up = (event.wheel.y > 0) != (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED);
            select_delta(*editor, up ? -int(page_size) : int(page_size));
            message = guidance;
            paint(renderer.get(), *editor, message);
        } else if (event.type == SDL_MOUSEBUTTONDOWN && event.button.windowID == id &&
                   event.button.button == SDL_BUTTON_LEFT) {
            unsigned page = (editor->selected / page_size) * page_size;
            SDL_Point point{event.button.x, event.button.y};
            for (unsigned i = page; i < std::min(page + page_size, editor->count); i++) {
                SDL_Rect bounds = cell(i - page);
                if (SDL_PointInRect(&point, &bounds)) {
                    editor->selected = i;
                    message = guidance;
                    paint(renderer.get(), *editor, message);
                    break;
                }
            }
        } else if (event.type == SDL_DROPFILE || event.type == SDL_DROPTEXT) {
            std::unique_ptr<char, decltype(&SDL_free)> path(event.drop.file, SDL_free);
            if (event.type == SDL_DROPFILE && event.drop.windowID == id && editor->count && path) {
                const char *error;
                try {
                    error = install(path.get(), editor->directory, editor->recent(editor->selected), max_texture_size);
                } catch (const std::exception&) {
                    error = "Could not import replacement";
                }
                changed |= !error;
                message = error ? error : "Saved! Replacement applies when gameplay resumes";
                SDL_Log("GLES2 texture editor: %s", message);
                paint(renderer.get(), *editor, message);
            }
        }
    }
    SDL_EventState(SDL_DROPFILE, drop_state);
    renderer.reset();
    window.reset();
    if (!quit)
        SDL_RaiseWindow(game);
    return static_cast<Gles2EditorResult>((changed ? GLES2_EDITOR_CHANGED : 0) | (quit ? GLES2_EDITOR_QUIT : 0));
}
