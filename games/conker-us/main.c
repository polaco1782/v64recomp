#include "game.h"
#include <setjmp.h>
#include "generated.h"
#include "audio.h"
#include "graphics.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "game_config.h"

typedef struct Game {
    Runtime runtime;
    RecompGameOptions options;
    RecompRenderer *renderer;
    RecompAudio *audio;
    uint8_t *rom;
    uint64_t now_ns, wall_origin, next_ns;
    bool reached_graphics;
    uint32_t boot_status;
    jmp_buf boot_return;
    bool booting;
} Game;

enum { GAME_BASE = 0x15000000, PAGE_BYTES = 0x1000, CODE_PAGES = 507, MANAGEMENT = 0x80043b40 };
static void initialize_services(Runtime *rt);
static void verify(Runtime *rt, uint32_t address, size_t size, const uint8_t expected[32]) {
    uint8_t *raw = malloc(size), digest[32];
    unsigned length = 0;
    recomp_game_require(raw != NULL, "Conker image verification allocation failed", address);
    for (size_t i = 0; i < size; i++)
        raw[i] = (uint8_t)recomp_load(rt, address + (uint32_t)i, 1);
    bool valid =
        EVP_Digest(raw, size, digest, &length, EVP_sha256(), NULL) && length == 32 && !memcmp(digest, expected, 32);
    free(raw);
    recomp_game_require(valid, "Conker loaded image fingerprint mismatch", address);
}
static uint32_t rom_word(Runtime *rt, uint32_t offset) {
    recomp_game_require(offset <= rt->rom_size - 4, "Conker page table outside ROM", offset);
    const uint8_t *p = rt->rom + offset;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint32_t page(Runtime *rt, unsigned index) {
    uint32_t entry = MANAGEMENT + index * 8;
    uint32_t physical = (uint32_t)recomp_load(rt, entry, 4);
    uint32_t base = (uint32_t)recomp_load(rt, 0x8003be74, 4);
    unsigned slots = (unsigned)recomp_load(rt, 0x8003be78, 2);
    recomp_game_require(slots && slots <= 0x400 && base && !(base % PAGE_BYTES), "Conker page cache not initialized",
                        base);
    recomp_physical_address(rt, base, slots * PAGE_BYTES);
    if (physical) {
        recomp_game_require(physical >= base && physical < base + slots * PAGE_BYTES && !(physical % PAGE_BYTES),
                            "Conker invalid resident page", entry);
        return physical;
    }
    uint32_t bitmap = (uint32_t)recomp_load(rt, 0x8003be70, 4);
    recomp_game_require(recomp_load(rt, 0x8003be7c, 4) == bitmap + (slots + 7) / 8 - 1,
                        "Conker free page bitmap bounds mismatch", bitmap);
    for (unsigned slot = 0; slot < slots; slot++) {
        uint32_t p = bitmap + slot / 8;
        unsigned bits = (unsigned)recomp_load(rt, p, 1), mask = 1u << (slot % 8);
        if (bits & mask) {
            recomp_store(rt, p, bits ^ mask, 1);
            physical = base + slot * PAGE_BYTES;
            break;
        }
    }
    if (!physical) {
        /* Original 0x10006138 scans through offset 0xfd8, including the sentinel.
         * It selects the largest signed pre-increment age and increments every age. */
        int32_t greatest = -1;
        uint32_t victim = 0;
        for (unsigned i = 0; i < 508; i++) {
            uint32_t p = MANAGEMENT + i * 8;
            uint32_t resident = (uint32_t)recomp_load(rt, p, 4);
            uint32_t age = (uint32_t)recomp_load(rt, p + 4, 4);
            if (resident && (int32_t)age > greatest) {
                greatest = (int32_t)age;
                victim = p;
                physical = resident;
            }
            recomp_store(rt, p + 4, age + 1, 4);
        }
        recomp_game_require(victim && physical >= base && physical < base + slots * PAGE_BYTES,
                            "Conker no valid page eviction candidate", entry);
        recomp_store(rt, victim, 0, 4);
    }
    uint32_t offsets = (uint32_t)recomp_load(rt, 0x800354fc, 4);
    uint32_t begin = (rom_word(rt, 0x42454 + index * 4) ^ 0x8039ccca) + 0x42450;
    uint32_t end = (rom_word(rt, 0x42458 + index * 4) ^ 0x8039ccca) + 0x42450;
    recomp_game_require(recomp_load(rt, offsets + index * 4, 4) == begin &&
                            recomp_load(rt, offsets + index * 4 + 4, 4) == end && begin < end && end <= rt->rom_size,
                        "Conker runtime page offsets differ from verified ROM", offsets + index * 4);
    unsigned bytes = index == CODE_PAGES - 1 ? 0x130 : PAGE_BYTES;
    recomp_game_require(rom_word(rt, begin) == bytes, "Conker decoded page length mismatch", begin);
    uint8_t decoded[PAGE_BYTES] = {0};
    z_stream stream = {.next_in = (Bytef *)(rt->rom + begin + 4),
                       .avail_in = end - begin - 4,
                       .next_out = decoded,
                       .avail_out = sizeof(decoded)};
    recomp_game_require(inflateInit2(&stream, -15) == Z_OK, "Conker page inflate initialization failed", begin);
    int result = inflate(&stream, Z_FINISH);
    bool valid = result == Z_STREAM_END && stream.total_out == bytes;
    inflateEnd(&stream);
    recomp_game_require(valid, "Conker code page decompression failed", begin);
    recomp_copy_to_ram(rt, physical, decoded, bytes);
    recomp_store(rt, entry, physical, 4);
    recomp_store(rt, entry + 4, 0, 4);
    return physical;
}
static bool resolve(Runtime *rt, uint32_t address, size_t bytes, bool write, uint32_t *physical) {
    if (address < GAME_BASE || address >= GAME_BASE + CONKER_GAME_SIZE)
        return false;
    recomp_game_require(rt->section_loaded[4] && !write, "Conker unloaded or writable ROM code page", address);
    uint32_t offset = address - GAME_BASE;
    recomp_game_require(bytes <= CONKER_GAME_SIZE - offset, "Conker virtual code span outside image", address);
    unsigned index = offset / PAGE_BYTES;
    uint32_t first = page(rt, index);
    *physical = first + offset % PAGE_BYTES;
    for (size_t span = PAGE_BYTES - offset % PAGE_BYTES; span < bytes; span += PAGE_BYTES) {
        index++;
        recomp_game_require(page(rt, index) == first + (index - offset / PAGE_BYTES) * PAGE_BYTES,
                            "Conker noncontiguous virtual code span", address);
    }
    return true;
}
static void prepare_call(Runtime *rt, uint32_t address) {
    if (address == 0x10022790 && !rt->scheduler)
        initialize_services(rt);
    if (address == 0x10005be0 && !rt->section_loaded[4]) {
        verify(rt, CONKER_GAME_DATA_ADDRESS, CONKER_GAME_DATA_SIZE, conker_game_data_sha256);
        rt->section_loaded[4] = rt->section_loaded[5] = true;
    }
    if (address >= GAME_BASE && address < GAME_BASE + CONKER_GAME_SIZE) {
        uint32_t physical;
        resolve(rt, address, 4, false, &physical);
    }
}
void recomp_conker_start_thread(Runtime *rt, MipsContext *ctx) {
    Game *game = (Game *)rt;
    if (game->booting)
        recomp_game_require((uint32_t)ctx->gpr[4] == 0x800318b0, "Conker unexpected boot thread",
                            (uint32_t)ctx->gpr[4]);
    recomp_os_start_thread(rt, ctx);
    if (game->booting)
        longjmp(game->boot_return, 1);
}
void recomp_conker_boot_tlb(Runtime *rt, MipsContext *ctx) {
    /* The cartridge's 4 MiB odd half is absent with the base 4 MiB memory model. */
    recomp_map_pages(rt, 0, 0x7fe000, 0x10000000, 0, UINT32_MAX);
    recomp_lookup(rt, 0x10001050)(rt, ctx);
}
void recomp_conker_unmap_range(Runtime *rt, MipsContext *ctx) {
    unsigned first = (uint32_t)ctx->gpr[4], last = (uint32_t)ctx->gpr[5];
    recomp_game_require(first <= last && last < 32, "Conker invalid unmap range", first);
    for (unsigned i = first; i <= last; i++)
        rt->page_mappings[i] = (RecompPageMapping){0};
}
void recomp_conker_get_status(Runtime *rt, MipsContext *ctx) {
    Game *game = (Game *)rt;
    mips_set(ctx, 2, mips_s32(game->boot_status));
}
void recomp_conker_set_status(Runtime *rt, MipsContext *ctx) {
    Game *game = (Game *)rt;
    uint32_t status = (uint32_t)ctx->gpr[4];
    recomp_game_require((status & 0x24000000) == 0x24000000, "Conker requires CU1 and independent FPRs", status);
    game->boot_status = status;
    ctx->fpu_fr1 = true;
    rt->interrupts_disabled = !(status & 1);
}
void recomp_conker_rom_read(Runtime *rt, MipsContext *ctx) {
    uint32_t offset = (uint32_t)ctx->gpr[4];
    uint32_t destination = (uint32_t)ctx->gpr[5];
    uint32_t bytes = ((uint32_t)ctx->gpr[6] + 1) & ~1u;
    recomp_game_require(!(offset & 1) && !(destination & 1) && bytes && offset < rt->rom_size &&
                            bytes <= rt->rom_size - offset,
                        "Conker invalid polled cartridge read", offset);
    recomp_game_require(!recomp_load(rt, 0x8003a573, 1), "Conker polled read while PI is busy", offset);
    recomp_store(rt, 0x8003a572, 1, 1);
    recomp_copy_to_ram(rt, destination, rt->rom + offset, bytes);
    recomp_store(rt, 0x8003a572, 0, 1);
    if (recomp_load(rt, 0x8003a575, 1))
        recomp_thread_start(rt, 0x80035910);
}
void recomp_conker_cache_init(Runtime *rt, MipsContext *ctx) {
    /* Replace privileged Context/Wired setup; preserve the original game allocator,
     * argument layout and allocation failure retry, rather than reserving guessed RAM. */
    unsigned count = (unsigned)ctx->gpr[4];
    unsigned previous = (unsigned)recomp_load(rt, 0x8003be78, 2);
    recomp_store(rt, 0x8003be80, 0x1fc, 2);
    recomp_store(rt, 0x8003be82, 0x3ff, 2);
    uint32_t sp = (uint32_t)ctx->gpr[29];
    MipsContext call = *ctx;
    call.gpr[29] = mips_s32(sp - 0x20);
    uint32_t physical;
    for (;;) {
        recomp_game_require(count > 0 && count <= 0x400, "Conker invalid page slot count", count);
        recomp_store(rt, 0x8003be78, count, 2);
        call.gpr[4] = count * PAGE_BYTES;
        call.gpr[5] = 0xff;
        call.gpr[6] = 4;
        call.gpr[7] = 1;
        recomp_store(rt, sp - 0x10, 2, 4);
        recomp_lookup(rt, 0x10003c6c)(rt, &call);
        physical = (uint32_t)call.gpr[2] & 0x0fffffff;
        if (physical)
            break;
        recomp_game_require(count != previous, "Conker page cache allocation failed", count);
        count = previous;
    }
    recomp_store(rt, 0x8003be74, physical, 4);
    call.gpr[4] = (count + 7) / 8;
    call.gpr[5] = 0xff;
    call.gpr[6] = call.gpr[7] = 0;
    recomp_lookup(rt, 0x10003c40)(rt, &call);
    recomp_game_require(call.gpr[2] != 0, "Conker page bitmap allocation failed", count);
    recomp_store(rt, 0x8003be70, call.gpr[2], 4);
    recomp_store(rt, 0x8003be7c, (uint32_t)call.gpr[2] + (count + 7) / 8 - 1, 4);
    mips_set(ctx, 2, call.gpr[2]);
}
static uint64_t guest_time(void *opaque) {
    Game *game = opaque;
    return game->now_ns + (game->wall_origin ? recomp_game_monotonic() - game->wall_origin : 0);
}
static bool submit(Runtime *rt, void *opaque, const RecompTask *task) {
    Game *game = opaque;
    (void)rt;
    if (task->words[0] == 2) {
        recomp_game_require(recomp_audio_submit(game->audio, task), "Conker unsupported audio task", task->words[4]);
        return true;
    }
    recomp_game_require(task->words[0] == 1, "Conker unsupported RSP task", task->words[0]);
    game->reached_graphics = true;
    return recomp_renderer_submit(game->renderer, task);
}
static void initialize_services(Runtime *rt) {
    Game *game = (Game *)rt;
    RecompClockConfig clock = {
        .ticks_per_second = BOOT_COUNTER_HZ, .now_ns = guest_time, .userdata = game, .timer_sentinel = 0x80042990};
    RecompSchedulerConfig scheduler = {.thread_tail = 0x8002bdf0,
                                       .run_queue = 0x8002bdf8,
                                       .active_queue = 0x8002bdfc,
                                       .running_thread = 0x8002be00,
                                       .idle_thread = 0x800318b0,
                                       .fpu_fr1 = true};
    RecompPlatformConfig platform = {.vi_period_ticks = 781250,
                                     .clock_rate = 0x8002bd10,
                                     .vi_clock = 0x8002bde8,
                                     .final_rom = 0x800428e0,
                                     .timer_list = 0x8002bd70,
                                     .current_time = 0x800429b0,
                                     .base_counter = 0x800429b8,
                                     .vi_intr_count = 0x800429bc,
                                     .app_nmi_buffer = 0x8000031c};
    recomp_game_require(recomp_clock_init(rt, &clock) && recomp_scheduler_init(rt, &scheduler) &&
                            recomp_platform_init(rt, &platform),
                        "Conker services initialization failed", 0);
    RecompAudioConfig audio = {.output_enabled = true,
                               .playback = !game->options.mute,
                               .microcode = RECOMP_AUDIO_NAUDIO_CBFD,
                               .task_profile = &conker_audio_task_profile};
    game->audio = recomp_game_audio_create(rt, &audio);
    recomp_game_require(game->audio != NULL, "Conker audio initialization failed", 0);
    /* Save setup at 0x1501c5d4 requires the probe's 16 Kbit device result (2). */
    RecompIOConfig io = {.connected_mask = 1,
                         .eeprom_size = 2048,
                         .eeprom_path = game->options.save,
                         .cont_init_queue = 0x80042a78,
                         .cont_init_messages = 0x80042a90,
                         .cont_init_queue_capacity = 1};
    RecompTaskBackend backend = {.userdata = game, .submit = submit};
    recomp_game_require(recomp_io_init(rt, &io) && recomp_tasks_init(rt, &backend),
                        "Conker IO/tasks initialization failed", 0);
}
static void game_init(Game *game, const RecompGameOptions *options) {
    *game = (Game){.options = *options, .boot_status = 0x34000001};
    Runtime *rt = &game->runtime;
    recomp_game_require(recomp_runtime_init(rt, BOOT_RAM_SIZE, recomp_functions, recomp_function_count,
                                            recomp_boot_sections, recomp_section_count),
                        "Conker runtime initialization failed", 0);
    rt->resolve_address = resolve;
    rt->prepare_call = prepare_call;
    game->rom = recomp_game_rom(rt, options, BOOT_ROM_SIZE, boot_sha1);
    recomp_copy_to_ram(rt, 0x80000000, game->rom, 0x1000);
    recomp_copy_to_ram(rt, BOOT_INITIAL_LOAD_ADDRESS, game->rom + BOOT_INITIAL_ROM_OFFSET, BOOT_INITIAL_LOAD_SIZE);
    for (size_t i = 0; i < sizeof(boot_parameters) / sizeof(boot_parameters[0]); i++)
        recomp_store(rt, boot_parameters[i][0], boot_parameters[i][1], 4);
    game->renderer = recomp_renderer_create_named(rt, options->data, "Conker's Bad Fur Day");
    recomp_game_require(game->renderer != NULL, "Conker renderer initialization failed", 0);
    recomp_renderer_set_microcode_hook(game->renderer, conker_bind_microcode);
    MipsContext boot = {.fcr31 = 0x01000800, .fpu_fr1 = true};
    boot.gpr[31] = mips_s32(0xf0000000);
    game->booting = true;
    if (!setjmp(game->boot_return))
        recomp_lookup(rt, BOOT_ENTRY)(rt, &boot);
    game->booting = false;
    game->next_ns = recomp_game_monotonic();
}
static void game_step(Game *game, const RecompPad *pad) {
    Runtime *rt = &game->runtime;
    recomp_game_require(recomp_game_input(rt, 0, pad), "Conker controller update failed", 0);
    for (unsigned i = 0; i < 256; i++)
        if (recomp_scheduler_run(rt, 16) != RECOMP_RUN_BUDGET)
            break;
    recomp_game_require(recomp_game_present(game->renderer), "Conker presentation failed", 0);
    if (!game->wall_origin) {
        game->now_ns += 16666667;
        if (game->reached_graphics)
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
