#include "player.h"
#include "codec.h"
#include <SDL.h>
#include <stdlib.h>
#include <stb_easy_font.h>
#ifdef _WIN32
#include <SDL_syswm.h>
#include <windows.h>
#include <commdlg.h>
#endif

typedef struct Player {
    MediaCodec codec;
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_AudioDeviceID device;
    SDL_AudioStream *stream;
    int stream_rate;
    bool paused, ended, started, muted;
    unsigned underruns;
    double seconds;
    char path[32768], status[160];
} Player;

static void text(SDL_Renderer *renderer, int x, int y, int scale, const char *message) {
    struct Vertex { float x, y, z; uint8_t color[4]; } vertices[8192];
    char ascii[256];
    size_t i;
    for (i = 0; message[i] && i < sizeof(ascii)-1; i++)
        ascii[i] = (unsigned char)message[i] >= 32 && (unsigned char)message[i] < 127 ? message[i] : '?';
    ascii[i] = 0;
    int quads = stb_easy_font_print(0, 0, ascii, NULL, vertices, sizeof(vertices));
    for (int q = 0; q < quads; q++) {
        struct Vertex *v = vertices+q*4;
        SDL_Rect rect = {x+(int)v[0].x*scale, y+(int)v[0].y*scale,
                        (int)(v[2].x-v[0].x)*scale, (int)(v[2].y-v[0].y)*scale};
        SDL_RenderFillRect(renderer, &rect);
    }
}

static void draw(Player *p) {
    SDL_SetRenderDrawColor(p->renderer, 16, 22, 32, 255);
    SDL_RenderClear(p->renderer);
    SDL_SetRenderDrawColor(p->renderer, 73, 219, 179, 255);
    text(p->renderer, 32, 32, 3, "N64 MP3 PLAYER");
    SDL_SetRenderDrawColor(p->renderer, 143, 157, 178, 255);
    text(p->renderer, 32, 76, 2, "Media Player V2 / ROM decoder");
    SDL_SetRenderDrawColor(p->renderer, 235, 240, 247, 255);
    const char *name = p->path;
    for (const char *s = p->path; *s; s++) if (*s == '/' || *s == '\\') name = s+1;
    char line[80];
    snprintf(line, sizeof(line), "%.52s", *name ? name : "Drop an MP3 here to begin");
    text(p->renderer, 32, 132, 2, line);
    SDL_SetRenderDrawColor(p->renderer, 143, 157, 178, 255);
    text(p->renderer, 32, 171, 2, p->status);
    double position = p->seconds - SDL_GetQueuedAudioSize(p->device)/192000.0;
    if (position < 0) position = 0;
    unsigned seconds = (unsigned)position;
    if (p->codec.frames) {
        snprintf(line, sizeof(line), "%02u:%02u    %d Hz  %s  %d kbps", seconds/60, seconds%60,
                 p->codec.rate, p->codec.channels == 1 ? "mono" : "stereo", p->codec.bitrate);
        text(p->renderer, 32, 211, 2, line);
    }
    SDL_SetRenderDrawColor(p->renderer, 35, 54, 66, 255);
    SDL_Rect buttons[] = {{32, 272, 182, 42}, {230, 272, 220, 42}, {466, 272, 222, 42}};
    for (int i = 0; i < 3; i++) SDL_RenderFillRect(p->renderer, &buttons[i]);
    SDL_SetRenderDrawColor(p->renderer, 235, 240, 247, 255);
    text(p->renderer, 46, 287, 2, "O  Open file");
    text(p->renderer, 244, 287, 2, p->paused ? "Space  Resume" : "Space  Pause");
    text(p->renderer, 480, 287, 2, "R  Restart");
    SDL_SetRenderDrawColor(p->renderer, 143, 157, 178, 255);
    text(p->renderer, 32, 338, 1, p->muted ? "M  Unmute     Esc  Exit" : "M  Mute       Esc  Exit");
    SDL_RenderPresent(p->renderer);
}

static void clear_audio(Player *p) {
    SDL_PauseAudioDevice(p->device, 1);
    SDL_ClearQueuedAudio(p->device);
    if (p->stream) SDL_FreeAudioStream(p->stream);
    p->stream = NULL; p->stream_rate = 0;
    p->paused = p->ended = p->started = false;
    p->seconds = 0; p->underruns = 0;
}

static bool open_file(Player *p, const char *path) {
    if (!media_codec_open(&p->codec, path)) {
        snprintf(p->status, sizeof(p->status), "Could not open file");
        return false;
    }
    clear_audio(p);
    snprintf(p->path, sizeof(p->path), "%s", path);
    SDL_SetWindowTitle(p->window, path);
    snprintf(p->status, sizeof(p->status), "Loading...");
    return true;
}

static void choose_file(Player *p) {
#ifdef _WIN32
    wchar_t path[32768] = {0};
    OPENFILENAMEW dialog = {0};
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (SDL_GetWindowWMInfo(p->window, &info)) dialog.hwndOwner = info.info.win.window;
    dialog.lStructSize = sizeof(dialog);
    dialog.lpstrFilter = L"MP3 audio\0*.mp3\0All files\0*.*\0";
    dialog.lpstrFile = path; dialog.nMaxFile = 32768;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    /* A modal file dialog must not drain the playback queue. */
    SDL_PauseAudioDevice(p->device, 1);
    if (GetOpenFileNameW(&dialog)) {
        char utf8[32768];
        if (WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof(utf8), NULL, NULL)) open_file(p, utf8);
    }
    if (p->started && !p->paused) SDL_PauseAudioDevice(p->device, 0);
#else
    snprintf(p->status, sizeof(p->status), "Drag an MP3 from your file manager");
#endif
}

static void queue_stream(Player *p) {
    int16_t buffer[8192];
    int bytes;
    while ((bytes = SDL_AudioStreamGet(p->stream, buffer, sizeof(buffer))) > 0) {
        if (p->muted) memset(buffer, 0, (size_t)bytes);
        recomp_game_require(SDL_QueueAudio(p->device, buffer, (Uint32)bytes) == 0, SDL_GetError(), 0);
    }
    recomp_game_require(bytes == 0, SDL_GetError(), 0);
}

int media_player_main(int argc, char **argv, MediaCapture capture) {
    RecompGameOptions options;
    int parsed = recomp_game_options(argc, argv, &options);
    if (parsed) return parsed < 0 ? 2 : 0;
    Player *p = calloc(1, sizeof(*p));
    recomp_game_require(p != NULL, "MP3 player allocation failed", 0);
    media_codec_init(&p->codec, &options);
    p->muted = options.mute;
    SDL_SetMainReady();
    recomp_game_require(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) == 0, SDL_GetError(), 0);
    p->window = SDL_CreateWindow("N64 MP3 Player", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 720, 372, SDL_WINDOW_SHOWN);
    recomp_game_require(p->window != NULL, SDL_GetError(), 0);
    p->renderer = SDL_CreateRenderer(p->window, -1, SDL_RENDERER_ACCELERATED);
    if (!p->renderer) p->renderer = SDL_CreateRenderer(p->window, -1, SDL_RENDERER_SOFTWARE);
    recomp_game_require(p->renderer != NULL, SDL_GetError(), 0);
    SDL_RenderSetLogicalSize(p->renderer, 720, 372);
    SDL_AudioSpec desired = {0};
    desired.freq = 48000; desired.format = AUDIO_S16SYS; desired.channels = 2; desired.samples = 1024;
    p->device = SDL_OpenAudioDevice(NULL, 0, &desired, NULL, 0);
    recomp_game_require(p->device != 0, SDL_GetError(), 0);
    snprintf(p->status, sizeof(p->status), "Choose a file or drag it onto this window");
    const char *initial = getenv("RECOMP_MEDIA_FILE");
    bool running = true;
    int result = 0;
    if (initial && *initial && !open_file(p, initial) && options.frames) { running = false; result = 2; }
    if (options.frames && (!initial || !*initial)) {
        fputs("Bounded MP3 runs require RECOMP_MEDIA_FILE\n", stderr);
        running = false; result = 2;
    }
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            SDL_Keycode key = 0;
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_DROPFILE) {
                if (open_file(p, event.drop.file)) result = 0;
                SDL_free(event.drop.file);
            }
            if (event.type == SDL_KEYDOWN && !event.key.repeat) key = event.key.keysym.sym;
            if (event.type == SDL_MOUSEBUTTONUP && event.button.button == SDL_BUTTON_LEFT &&
                event.button.y >= 272 && event.button.y < 314) {
                int x = event.button.x;
                key = x >= 32 && x < 214 ? SDLK_o : x >= 230 && x < 450 ? SDLK_SPACE :
                      x >= 466 && x < 688 ? SDLK_r : 0;
            }
            if (key == SDLK_ESCAPE) running = false;
            if (key == SDLK_o) { choose_file(p); if (!p->ended) result = 0; }
            if (key == SDLK_SPACE && p->codec.file) {
                p->paused = !p->paused;
                SDL_PauseAudioDevice(p->device, p->paused || !p->started);
            }
            if (key == SDLK_r && media_codec_restart(&p->codec)) { clear_audio(p); result = 0; }
            if (key == SDLK_m) p->muted = !p->muted;
        }
        Uint32 queued = SDL_GetQueuedAudioSize(p->device);
        if (p->started && !p->paused && !p->ended && !queued) p->underruns++;
        for (int burst = 0; running && p->codec.file && !p->ended && !p->paused &&
             SDL_GetQueuedAudioSize(p->device) < 76800 && burst < 16; burst++) {
            int decoded = media_codec_step(&p->codec);
            if (decoded < 0) {
                p->ended = true;
                if (p->stream) { SDL_AudioStreamFlush(p->stream); queue_stream(p); }
                if (decoded == -2) { snprintf(p->status, sizeof(p->status), "File read failed"); result = 2; }
                else if (!p->codec.frames) { snprintf(p->status, sizeof(p->status), "No playable MP3 frames found"); result = 2; }
                break;
            }
            if (!decoded) continue;
            if (capture) capture(p->codec.pcm, p->codec.samples, p->codec.rate);
            if (p->stream_rate != p->codec.rate) {
                if (p->stream) {
                    SDL_AudioStreamFlush(p->stream); queue_stream(p); SDL_FreeAudioStream(p->stream);
                }
                p->stream = SDL_NewAudioStream(AUDIO_S16SYS, 2, p->codec.rate, AUDIO_S16SYS, 2, 48000);
                recomp_game_require(p->stream != NULL, SDL_GetError(), 0);
                p->stream_rate = p->codec.rate;
            }
            recomp_game_require(SDL_AudioStreamPut(p->stream, p->codec.pcm, p->codec.samples*4) == 0, SDL_GetError(), 0);
            queue_stream(p);
            p->seconds += (double)p->codec.samples/p->codec.rate;
            if (options.frames && p->codec.frames >= options.frames) {
                p->ended = true; SDL_AudioStreamFlush(p->stream); queue_stream(p);
            }
        }
        queued = SDL_GetQueuedAudioSize(p->device);
        if (!p->started && !p->paused && (queued >= 38400 || (p->ended && queued))) {
            p->started = true; SDL_PauseAudioDevice(p->device, 0);
        }
        if (p->codec.frames && result == 0)
            snprintf(p->status, sizeof(p->status), "%s%s", p->paused ? "Paused" :
                     p->ended && !queued ? "Finished - press R to play again" : "Playing", p->muted ? "  (muted)" : "");
        if (p->ended && !queued && options.frames) running = false;
        draw(p);
        SDL_Delay(8);
    }
#ifdef RECOMP_DEBUG
    fprintf(stderr, "MP3: frames=%llu seconds=%.3f underruns=%u\n",
            (unsigned long long)p->codec.frames, p->seconds, p->underruns);
    const char *snapshot = getenv("RECOMP_MEDIA_SNAPSHOT");
    if (snapshot) {
        SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, 720, 372, 32, SDL_PIXELFORMAT_ARGB8888);
        recomp_game_require(surface != NULL, SDL_GetError(), 0);
        recomp_game_require(SDL_RenderReadPixels(p->renderer, NULL, surface->format->format,
                            surface->pixels, surface->pitch) == 0 && SDL_SaveBMP(surface, snapshot) == 0,
                            "MP3 screenshot failed", 0);
        SDL_FreeSurface(surface);
    }
#endif
    if (p->stream) SDL_FreeAudioStream(p->stream);
    SDL_CloseAudioDevice(p->device);
    SDL_DestroyRenderer(p->renderer); SDL_DestroyWindow(p->window); SDL_Quit();
    media_codec_close(&p->codec); free(p);
    return result;
}
