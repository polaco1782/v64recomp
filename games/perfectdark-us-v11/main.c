#define _POSIX_C_SOURCE 200809L
#include "game.h"
#include "generated.h"
#include "renderer.h"
#include "recomp_audio.h"
#include "audio.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game_config.h"

typedef struct Game {
    Runtime runtime; /* First member so guest service callbacks can find their owner. */
    RecompGameOptions options;
    RecompRenderer *renderer;
    RecompAudio *audio;
    uint8_t *rom;
    uint64_t now_ns, wall_origin, next_ns;
    unsigned graphics_tasks, audio_tasks;
} Game;
static void initialize_services(Runtime *rt);
static void verify(Runtime *, uint32_t, size_t, const uint8_t[32]);
static void prepare_call(Runtime *rt, uint32_t address) {
    if (address == 0x70005b60 && !rt->scheduler)
        initialize_services(rt);
    if (address == 0x7004e1b0 && !rt->section_loaded[4]) {
        const MipsContext *ctx = recomp_thread_context(rt, 0);
        if ((uint32_t)ctx->gpr[6] == 0x7f1a0000) {
            uint32_t base = ((uint32_t)ctx->gpr[7] - 0x1a0000) | 0x80000000;
            verify(rt, base, PD_GAME_SIZE, pd_game_sha256);
            rt->section_loaded[4] = true;
        }
    }
}
static void verify(Runtime *rt, uint32_t address, size_t size, const uint8_t expected[32]) {
    uint8_t *raw = malloc(size), digest[32];
    unsigned length = 0;
    recomp_game_require(raw != NULL, "Perfect Dark verification allocation failed", address);
    for (size_t i = 0; i < size; i++)
        raw[i] = (uint8_t)recomp_load(rt, address + (uint32_t)i, 1);
    bool valid =
        EVP_Digest(raw, size, digest, &length, EVP_sha256(), NULL) && length == 32 && !memcmp(digest, expected, 32);
    free(raw);
    recomp_game_require(valid, "Perfect Dark loaded image fingerprint mismatch", address);
}
void recomp_pd_boot_tlb(Runtime *rt, MipsContext *ctx) {
    recomp_map_pages(rt, 0, 0x7fe000, 0x70000000, 0, 0x400000);
    recomp_lookup(rt, 0x700016cc)(rt, ctx);
}
void recomp_pd_unmap_range(Runtime *rt, MipsContext *ctx) {
    unsigned first = (uint32_t)ctx->gpr[4], last = (uint32_t)ctx->gpr[5];
    recomp_game_require(first <= last && last < 32, "Perfect Dark invalid TLB range", first);
    for (unsigned i = first; i <= last; i++)
        rt->page_mappings[i] = (RecompPageMapping){0};
}
void recomp_pd_boot_rom_read(Runtime *rt, MipsContext *ctx) {
    /* mtx_get_obfuscated_rom_base serializes a raw PI read of the immutable
     * boot ROM word. Native DMA is already serialized by the runtime. */
    uint32_t base = (uint32_t)recomp_load(rt, 0x80000308, 4);
    uint32_t address = (base | 0xb764b4fd) ^ 0x0764bea1;
    recomp_game_require(address >= 0xb0000000 && address - 0xb0000000 <= rt->rom_size - 4,
                        "Perfect Dark boot ROM read outside cartridge", address);
    const uint8_t *p = rt->rom + address - 0xb0000000;
    uint32_t value = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    mips_set(ctx, 2, mips_s32(value));
}
void recomp_pd_enter_inflate(Runtime *rt, MipsContext *ctx) {
    if (!rt->section_loaded[2]) {
        verify(rt, PD_INFLATE_ADDRESS, PD_INFLATE_SIZE, pd_inflate_sha256);
        rt->section_loaded[2] = true;
    }
    mips_set(ctx, 7, 0x7020126c);
    recomp_lookup(rt, 0x7020126c)(rt, ctx);
}
static uint64_t guest_time(void *opaque) {
    Game *game = opaque;
    return game->wall_origin ? game->now_ns + recomp_game_monotonic() - game->wall_origin : game->now_ns;
}
static bool submit(Runtime *rt, void *opaque, const RecompTask *task) {
    (void)rt;
    Game *game = opaque;
    if (task->words[0] == 2) {
        game->audio_tasks++;
        recomp_game_require(recomp_audio_submit(game->audio, task), "Perfect Dark unsupported audio task",
                            task->words[4]);
    } else {
        recomp_game_require(task->words[0] == 1, "Perfect Dark unsupported RSP task", task->words[0]);
        game->graphics_tasks++;
        recomp_game_require(recomp_renderer_submit(game->renderer, task), "Perfect Dark graphics task rejected",
                            task->words[4]);
    }
    return true;
}
static void initialize_services(Runtime *rt) {
    /* Runtime is the first member of the owning game state. */
    Game *game = (Game *)rt;
    verify(rt, PD_LIB_ADDRESS, PD_LIB_SIZE, pd_lib_sha256);
    verify(rt, PD_DATA_ADDRESS, PD_DATA_SIZE, pd_data_sha256);
    rt->section_loaded[3] = true;
    RecompClockConfig clock = {
        .ticks_per_second = BOOT_COUNTER_HZ, .now_ns = guest_time, .userdata = game, .timer_sentinel = 0x8009c760};
    RecompSchedulerConfig scheduler = {.thread_tail = 0x80060930,
                                       .run_queue = 0x80060938,
                                       .active_queue = 0x8006093c,
                                       .running_thread = 0x80060940,
                                       .idle_thread = 0x8008d4a0};
    RecompPlatformConfig platform = {.vi_period_ticks = 781250,
                                     .clock_rate = 0x8005cf60,
                                     .vi_clock = 0x8005cf68,
                                     .final_rom = 0x80090a60,
                                     .timer_list = 0x800608a0,
                                     .current_time = 0x8009c780,
                                     .base_counter = 0x8009c788,
                                     .vi_intr_count = 0x8009c78c,
                                     .app_nmi_buffer = 0x8000031c};
    recomp_game_require(recomp_clock_init(rt, &clock) && recomp_scheduler_init(rt, &scheduler) &&
                            recomp_platform_init(rt, &platform),
                        "Perfect Dark services init failed", 0);
    RecompIOConfig io = {
        .connected_mask = 1, .eeprom_size = 2048, .eeprom_path = game->options.save, .si_transfer_ticks = 1000};
    RecompTaskBackend backend = {.userdata = game, .submit = submit};
    recomp_game_require(recomp_io_init(rt, &io) && recomp_tasks_init(rt, &backend), "Perfect Dark IO/task init failed",
                        0);
    RecompAudioConfig ac = {.output_enabled = true,
                            .playback = !game->options.mute,
                            .prebuffer_ms = 160,
                            .microcode = RECOMP_AUDIO_NAUDIO_MP3,
                            .task_profile = &perfectdark_audio_task_profile};
    game->audio = recomp_game_audio_create(rt, &ac);
    recomp_game_require(game->audio != NULL, "Perfect Dark audio initialization failed", 0);
}
static void game_init(Game *game, const RecompGameOptions *options) {
    *game = (Game){.options = *options};
    Runtime *rt = &game->runtime;
    recomp_game_require(recomp_runtime_init(rt, BOOT_RAM_SIZE, recomp_functions, recomp_function_count,
                                            recomp_boot_sections, recomp_section_count),
                        "Perfect Dark runtime init failed", 0);
    rt->prepare_call = prepare_call;
    game->rom = recomp_game_rom(rt, options, BOOT_ROM_SIZE, boot_sha1);
    recomp_copy_to_ram(rt, 0x80000000, game->rom, 0x1000);
    recomp_copy_to_ram(rt, BOOT_INITIAL_LOAD_ADDRESS, game->rom + BOOT_INITIAL_ROM_OFFSET, BOOT_INITIAL_LOAD_SIZE);
    for (size_t i = 0; i < sizeof(boot_parameters) / sizeof(boot_parameters[0]); i++)
        recomp_store(rt, boot_parameters[i][0], boot_parameters[i][1], 4);
    game->renderer = recomp_renderer_create_named(rt, "Perfect Dark");
    recomp_game_require(game->renderer != NULL, "Perfect Dark renderer initialization failed", 0);
    recomp_renderer_set_framebuffer_sizes(game->renderer, game_graphics_framebuffers,
                                          sizeof(game_graphics_framebuffers) / sizeof(game_graphics_framebuffers[0]));
    recomp_renderer_set_graphics_microcodes(game->renderer, game_graphics_microcodes,
                                            sizeof(game_graphics_microcodes) / sizeof(game_graphics_microcodes[0]));
    MipsContext boot = {.fcr31 = 0x01000800};
    boot.gpr[31] = mips_s32(0xf0000000);
    recomp_lookup(rt, BOOT_ENTRY)(rt, &boot);
    game->next_ns = recomp_game_monotonic();
}
static void game_step(Game *game, const RecompPad *pad) {
    Runtime *rt = &game->runtime;
    recomp_game_require(recomp_game_input(rt, 0, pad), "Perfect Dark controller update failed", 0);
    for (unsigned i = 0; i < 256; i++)
        if (recomp_scheduler_run(rt, 16) != RECOMP_RUN_BUDGET)
            break;
    recomp_game_require(recomp_game_present(game->renderer), "Perfect Dark presentation failed", 0);
    if (!game->wall_origin) {
        game->now_ns += 16666667;
        if (game->graphics_tasks)
            game->wall_origin = recomp_game_monotonic();
    }
    game->next_ns += 16666667;
    /* Keep the deterministic boot clock until the first graphics handoff. */
    if (game->wall_origin)
        recomp_game_pace(rt, game->next_ns);
    else
        recomp_game_sleep(game->next_ns);
}
static void game_destroy(Game *game) {
#ifdef RECOMP_DEBUG
    fprintf(stderr, "Perfect Dark: %u graphics tasks, %u audio tasks\n", game->graphics_tasks, game->audio_tasks);
#endif
    recomp_audio_destroy(game->audio);
    recomp_renderer_destroy(game->renderer);
    recomp_runtime_destroy(&game->runtime);
    free(game->rom);
}

int main(int argc, char **argv) {
    RecompGameOptions options;
    int parsed = recomp_game_options(argc, argv, &options);
    if (parsed)
        return parsed < 0 ? 2 : 0;
    Game game;
    game_init(&game, &options);
    for (uint32_t frame = 0; !options.frames || frame < options.frames; frame++) {
        RecompPad pad = {.connected = true};
        if (!recomp_renderer_input(game.renderer, &pad))
            break;
        game_step(&game, &pad);
    }
    game_destroy(&game);
    return 0;
}
