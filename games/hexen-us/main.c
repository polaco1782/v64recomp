#define _POSIX_C_SOURCE 200809L
#include "game.h"
#include "graphics.h"
#include "audio.h"
#include "generated.h"
#include "game_config.h"
#include "hexen_input.h"
#include "fov.h"
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Game {
    Runtime runtime;
    uint8_t *rom;
    RecompRenderer *renderer;
    RecompAudio *audio;
    uint64_t wall_origin;
    bool program_verified, helper_verified, running;
} Game;
static Game *game;

static bool game_poll_input(Game *g, RecompPad *pad) {
    recomp_renderer_mouse_mode(g->renderer, hexen_input_enabled(),
                               g->program_verified && hexen_input_playable(&g->runtime));
    if (!g->running || !recomp_renderer_input(g->renderer, pad)) {
        g->running = false;
        hexen_input_submit((RecompMouseFrame){0}, pad);
        return false;
    }
    hexen_input_submit(recomp_renderer_mouse_frame(g->renderer), pad);
    return true;
}
static void poll_command_input(Runtime *rt, void *opaque) {
    (void)opaque;
    RecompPad pad = {.connected = true};
    if (game_poll_input(game, &pad))
        recomp_game_require(recomp_game_input(rt, 0, &pad), "Hexen controller update failed", 0);
}

static void check_image(Runtime *rt, uint32_t address, uint32_t size, const uint8_t expected[32]) {
    uint8_t *raw = malloc(size), digest[32];
    recomp_game_require(raw != NULL, "Hexen image check allocation failed", address);
    for (uint32_t i = 0; i < size; i++)
        raw[i] = (uint8_t)recomp_load(rt, address + i, 1);
    SHA256(raw, size, digest);
    free(raw);
    recomp_game_require(!memcmp(digest, expected, 32), "Hexen guest decoded image mismatch", address);
}
static void prepare_call(Runtime *rt, uint32_t address) {
    if (address == BOOT_MAIN_HANDOFF_CALL) {
        /* The first real thread has taken the token. Join IPL before upper
         * RAM becomes guest graphics/task storage. */
        RecompThreadSnapshot threads[16];
        size_t count = recomp_thread_snapshots(rt, threads, 16);
        for (size_t i = 0; i < count && i < 16; i++)
            if (threads[i].current && threads[i].object == BOOT_MAIN_OBJECT)
                recomp_thread_destroy(rt, 0x803ff000);
    }
    if (address == 0x8004d078 && !game->program_verified) {
        /* LZH tree reconstruction calls the game's memmove after its bytes
         * have been decoded. The section is partially resident until return;
         * guard every entry below, retaining the core loaded-section checks. */
        rt->section_loaded[BOOT_GAME_SECTION] = true;
    }
    if (!game->program_verified && address >= BOOT_DECODED_ADDRESS &&
        address < BOOT_DECODED_ADDRESS + BOOT_DECODED_SIZE) {
        if (address == 0x800dc9b0) {
            /* Full helper body is checked against the prepared ROM image. */
            if (!game->helper_verified) {
                check_image(rt, address, 0x78, memmove_sha256);
                game->helper_verified = true;
            }
        } else {
            check_image(rt, BOOT_DECODED_ADDRESS, BOOT_DECODED_SIZE, decoded_sha256);
            game->program_verified = true;
        }
    }
}
static uint64_t guest_time(void *opaque) {
    Game *g = opaque;
    return recomp_game_monotonic() - g->wall_origin;
}
static bool submit(Runtime *rt, void *opaque, const RecompTask *task) {
    (void)rt;
    Game *g = opaque;
    recomp_game_require(task->words[0] == 1 || task->words[0] == 2, "Hexen unsupported RSP task type",
                        task->guest_address);
    RecompTask normalized = *task;
    if (task->words[0] == 1) {
        recomp_game_require(task->words[13] <= UINT32_MAX / sizeof(uint64_t),
                            "Hexen display-list command count overflow", task->guest_address);
        normalized.words[13] *= sizeof(uint64_t);
    }
    bool ok =
        task->words[0] == 2 ? recomp_audio_submit(g->audio, task) : recomp_renderer_submit(g->renderer, &normalized);
    recomp_game_require(ok, "Hexen RSP task rejected", task->words[4]);
    return true;
}
static void game_init(Game *g, const RecompGameOptions *options) {
    memset(g, 0, sizeof(*g));
    game = g;
    hexen_fov_init();
    Runtime *rt = &g->runtime;
    recomp_game_require(recomp_runtime_init(rt, BOOT_RAM_SIZE, recomp_functions, recomp_function_count,
                                            recomp_boot_sections, recomp_section_count),
                        "Hexen runtime initialization failed", 0);
    g->rom = recomp_game_rom(rt, options, BOOT_ROM_SIZE, boot_sha1);
    rt->prepare_call = prepare_call;
    recomp_copy_to_ram(rt, BOOT_INITIAL_LOAD_ADDRESS, g->rom + BOOT_INITIAL_ROM_OFFSET, BOOT_INITIAL_LOAD_SIZE);
    for (size_t i = 0; i < sizeof(boot_parameters) / sizeof(boot_parameters[0]); i++)
        recomp_store(rt, boot_parameters[i][0], boot_parameters[i][1], 4);
    g->wall_origin = recomp_game_monotonic();
    RecompClockConfig clock = {.ticks_per_second = BOOT_COUNTER_HZ, .now_ns = guest_time, .userdata = g};
    RecompSchedulerConfig scheduler = hexen_scheduler_config;
    scheduler.idle_thread = BOOT_MAIN_OBJECT;
    static const RecompBootLoad loads[] = {
        {BOOT_SUPPORT_SECTION, BOOT_SUPPORT_ADDRESS, BOOT_SUPPORT_ROM_OFFSET, BOOT_SUPPORT_SIZE}};
    RecompPlatformConfig platform = hexen_platform_config;
    platform.vi_period_ticks = 781250;
    platform.code_loads = loads;
    platform.code_load_count = 1;
    recomp_game_require(recomp_clock_init(rt, &clock) && recomp_scheduler_init(rt, &scheduler) &&
                            recomp_platform_init(rt, &platform),
                        "Hexen native services initialization failed", 0);
    RecompIOConfig io = {.connected_mask = 1, .pak_mask = 1, .pak_paths = {options->save}};
    RecompTaskBackend backend = {.submit = submit, .userdata = g};
    recomp_game_require(recomp_io_init(rt, &io) && recomp_tasks_init(rt, &backend),
                        "Hexen IO/tasks initialization failed", 0);
    g->renderer = recomp_renderer_create_named(rt, options->data, "Hexen");
    recomp_game_require(g->renderer != NULL, "Hexen renderer initialization failed", 0);
    recomp_renderer_set_fov_hook(g->renderer, hexen_fov_adjust);
    g->running = true;
    hexen_input_init(poll_command_input);
    recomp_renderer_set_microcode_hook(g->renderer, hexen_bind_microcode);
    RecompAudioConfig ac = {.output_enabled = true,
                            .playback = !options->mute,
                            .prebuffer_ms = 160,
                            .microcode = RECOMP_AUDIO_ABI1,
                            .task_profile = &hexen_audio_task_profile};
    g->audio = recomp_game_audio_create(rt, &ac);
    recomp_game_require(g->audio != NULL, "Hexen audio initialization failed", 0);
    recomp_thread_create(rt, 0x803ff000, -1, BOOT_ENTRY, 0, 0x803fd000, 0);
    recomp_thread_start(rt, 0x803ff000);
}
static void game_step(Game *g, const RecompPad *pad) {
    Runtime *rt = &g->runtime;
    if (pad)
        recomp_game_require(recomp_game_input(rt, 0, pad), "Hexen controller update failed", 0);
    for (unsigned batch = 0; g->running && batch < 16; batch++) {
        if (recomp_scheduler_run(rt, 16) != RECOMP_RUN_BUDGET)
            break;
        RecompThreadSnapshot threads[32];
        size_t count = recomp_thread_snapshots(rt, threads, 32);
        bool work = false;
        for (size_t i = 0; i < count && i < 32; i++)
            if (threads[i].state == RECOMP_READY && threads[i].object != BOOT_MAIN_OBJECT)
                work = true;
        if (!work)
            break;
    }
}
static void game_pace(Game *g, uint64_t deadline) {
    recomp_game_pace(&g->runtime, deadline);
}
static void game_destroy(Game *g) {
    recomp_audio_destroy(g->audio);
    recomp_renderer_destroy(g->renderer);
    recomp_runtime_destroy(&g->runtime);
    free(g->rom);
    game = NULL;
}

int main(int argc, char **argv) {
    RecompGameOptions options;
    int parsed = recomp_game_options(argc, argv, &options);
    if (parsed)
        return parsed < 0 ? 2 : 0;
    Game game;
    game_init(&game, &options);
    uint64_t deadline = recomp_game_monotonic();
    for (uint32_t frame = 0; !options.frames || frame < options.frames; frame++) {
        RecompPad pad = {.connected = true};
        if (!game_poll_input(&game, &pad))
            break;
        game_step(&game, &pad);
        if (!game.running)
            break;
        recomp_game_require(recomp_game_present(game.renderer), "Hexen presentation failed", 0);
        deadline += 16666667;
        game_pace(&game, deadline);
    }
    game_destroy(&game);
    return 0;
}
