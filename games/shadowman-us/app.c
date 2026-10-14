#define _GNU_SOURCE
#include "runtime.h"
#include "generated.h"
#include "shadowman_config.h"
#include "pager.h"
#include <dlfcn.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef RECOMP_WITH_RT64
#include "renderer.h"
#include "audio.h"
#endif

/* Set to 1 to trace registered guest calls during ROM bring-up. */
#define LOG_SHADOWMAN_CALLS 0
static bool boot_bss_cleared, system_bss_cleared, tlb_installed;
static bool application_started;
static const char *report_path;
static uint32_t recent_calls[128];
static unsigned call_cursor;
static uint32_t interrupt_mask = 0x3fff01;
static uint64_t now_ns;
static bool reached_task;
static RecompTask first_task;
#ifdef RECOMP_WITH_RT64
static RecompRenderer *renderer;
static RecompAudio *audio;
static unsigned graphics_tasks, presentations;
static bool window_closed;
static uint64_t wall_origin;
static FILE *input_script;
static bool next_input;
static unsigned next_input_frame;
static RecompPad replay_pad = {.connected=true}, pending_pad = {.connected=true};
static void read_input(void) {
    unsigned buttons; int x, y;
    if (fscanf(input_script, "%u %u %d %d", &next_input_frame, &buttons, &x, &y) != 4) {
        next_input = false; return;
    }
    pending_pad.buttons = (uint16_t)buttons;
    pending_pad.stick_x = (int8_t)x;
    pending_pad.stick_y = (int8_t)y;
    next_input = true;
}
static uint64_t monotonic_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) recomp_fail("Shadow Man host clock failed", 0);
    return (uint64_t)ts.tv_sec*UINT64_C(1000000000)+(uint64_t)ts.tv_nsec;
}
static void pace(uint64_t deadline) {
    uint64_t now = monotonic_ns();
    if (deadline > now) {
        uint64_t delta = deadline-now;
        struct timespec ts = {(time_t)(delta/1000000000), (long)(delta%1000000000)};
        nanosleep(&ts, NULL);
    }
}
#endif

static void require(bool condition, const char *message, uint32_t address) {
    if (!condition) recomp_fail(message, address);
}
static void check_zero(Runtime *rt, uint32_t start, uint32_t end) {
    for (uint32_t a = start; a < end; a += 4)
        require(recomp_load(rt, a, 4) == 0, "Shadow Man BSS was not cleared", a);
}
static void prepare_call(Runtime *rt, uint32_t address) {
    recent_calls[call_cursor++ % 128] = address;
    if (LOG_SHADOWMAN_CALLS) fprintf(stderr, "Shadow Man call 0x%08x\n", address);
    if (address >= BOOT_GAME_VRAM && address < BOOT_GAME_VRAM + BOOT_GAME_SIZE) {
        for (unsigned i = 0; i < 16; i++) {
            uint8_t expected = rt->rom[BOOT_GAME_ROM_OFFSET + address - BOOT_GAME_VRAM + i];
            require(recomp_load(rt, address+i, 1) == expected,
                    "Shadow Man executable page differs from translated ROM", address+i);
        }
    }
}
void hook_shadowman_cache(Runtime *rt, MipsContext *ctx) { (void)rt; (void)ctx; }
void hook_shadowman_int_mask(Runtime *rt, MipsContext *ctx) {
    uint32_t previous = interrupt_mask;
    interrupt_mask = (uint32_t)ctx->gpr[4];
    rt->interrupts_disabled = !(interrupt_mask & 1);
    mips_set(ctx, 2, mips_s32(previous));
}
void hook_shadowman_ai_busy(Runtime *rt, MipsContext *ctx) {
    mips_set(ctx, 2, recomp_ai_state(rt).queued == 2 ? 1 : 0);
}
void hook_shadowman_map_tlb(Runtime *rt, MipsContext *ctx) {
    uint32_t index = (uint32_t)ctx->gpr[4], sp = (uint32_t)ctx->gpr[29];
    uint32_t even = (uint32_t)ctx->gpr[7], odd = (uint32_t)recomp_load(rt, sp+16, 4);
    require(index < 32 && recomp_load(rt, sp+20, 4) == 7,
            "Shadow Man TLB requires a global SDK mapping", ctx->pc);
    if (even == UINT32_MAX && odd == UINT32_MAX) rt->page_mappings[index] = (RecompPageMapping){0};
    else recomp_map_pages(rt, index, (uint32_t)ctx->gpr[5], (uint32_t)ctx->gpr[6], even, odd);
}
void hook_shadowman_unmap_tlb(Runtime *rt, MipsContext *ctx) {
    unsigned index = (unsigned)ctx->gpr[4];
    require(index < 32, "Shadow Man invalid TLB index", index);
    rt->page_mappings[index] = (RecompPageMapping){0};
}
void hook_shadowman_no_pak(Runtime *rt, MipsContext *ctx) {
    (void)rt;
    mips_set(ctx, 2, 2); /* PFS_ERR_NOPACK: the configured controller has no accessory. */
}
void hook_shadowman_motor_init(Runtime *rt, MipsContext *ctx) {
    uint32_t pfs = (uint32_t)ctx->gpr[5], channel = (uint32_t)ctx->gpr[6];
    require(channel < 4, "Shadow Man invalid motor channel", channel);
    recomp_store(rt, pfs, 0, 4);
    recomp_store(rt, pfs+4, ctx->gpr[4], 4);
    recomp_store(rt, pfs+8, channel, 4);
    recomp_store(rt, pfs+0x65, 0x80, 1);
    mips_set(ctx, 2, 2);
}
void hook_shadowman_motor_stop(Runtime *rt, MipsContext *ctx) {
    (void)rt;
    mips_set(ctx, 2, 5); /* PFS_ERR_INVALID: osMotorInit could not find an accessory. */
}
void hook_shadowman_pi_raw_dma(Runtime *rt, MipsContext *ctx) {
    uint32_t offset = (uint32_t)ctx->gpr[5], address = (uint32_t)ctx->gpr[6], bytes = (uint32_t)ctx->gpr[7];
    if (offset >= 0xb0000000 && offset < 0xb2000000) offset -= 0xb0000000;
    if (offset >= 0x10000000 && offset < 0x12000000) offset -= 0x10000000;
    require(ctx->gpr[4] == 0 && offset <= rt->rom_size && bytes <= rt->rom_size-offset,
            "Shadow Man unsupported raw PI DMA", offset);
    recomp_copy_to_ram(rt, address, rt->rom+offset, bytes);
    if (offset == BOOT_GAME_ROM_OFFSET && bytes >= ((BOOT_GAME_SIZE-1) & ~0xfffu))
        rt->section_loaded[BOOT_GAME_SECTION] = true;
    require(recomp_event_post(rt, 8), "Shadow Man PI completion FIFO full", address);
    mips_set(ctx, 2, 0);
}
void hook_shadowman_boot_tlb(Runtime *rt, MipsContext *ctx) {
    require(!tlb_installed && ctx->gpr[4] == 30, "Shadow Man boot TLB ABI mismatch", ctx->pc);
    check_zero(rt, BOOT_BOOT_BSS_START, BOOT_BOOT_BSS_END);
    require(recomp_load(rt, BOOT_BOOT_BSS_END, 4) == 0xa5a5a5a5,
            "Shadow Man initial BSS clear overran", BOOT_BOOT_BSS_END);
    boot_bss_cleared = true;
    /* Original inline unmap loop followed by a global 1 MiB odd-page mapping.
     * EntryLo global bits make the SDK's ASID argument immaterial here. */
    recomp_unmap_pages(rt);
    recomp_map_pages(rt, 31, 0x1fe000, 0, UINT32_MAX, 0);
    require(recomp_ram_address(rt, BOOT_SYSTEM_ENTRY, 4) == BOOT_SYSTEM_ENTRY - 0x100000,
            "Shadow Man low virtual mapping mismatch", BOOT_SYSTEM_ENTRY);
    tlb_installed = true;
    mips_set(ctx, 31, mips_s32(0x800004ac));
    recomp_lookup(rt, BOOT_SYSTEM_ENTRY)(rt, ctx);
    recomp_fail("Shadow Man system startup returned", BOOT_SYSTEM_ENTRY);
}
void hook_shadowman_get_sr(Runtime *rt, MipsContext *ctx) {
    mips_set(ctx, 2, 0x2400ff00 | (rt->interrupts_disabled ? 0 : 1));
}
void hook_shadowman_start_thread(Runtime *rt, MipsContext *ctx) {
    hook_os_start_thread(rt, ctx);
    if (!application_started) {
        application_started = true;
        /* IPL has no schedulable thread on hardware. Remove our host-only
         * transfer thread when the original first thread is dispatched. */
        recomp_thread_stop(rt, 0);
        recomp_fail("Shadow Man IPL thread resumed", ctx->pc);
    }
}
void hook_shadowman_initialize(Runtime *rt, MipsContext *ctx) {
    require(tlb_installed && !system_bss_cleared, "Shadow Man repeated OS initialization", ctx->pc);
    check_zero(rt, BOOT_BSS_START, BOOT_BSS_END);
    require(recomp_load(rt, BOOT_BSS_END, 4) == 0xa5a5a5a5,
            "Shadow Man system BSS clear overran", BOOT_BSS_END);
    system_bss_cleared = true;
    hook_os_initialize(rt, ctx);
    recomp_store(rt, 0x800285c0, 48681812, 4); /* Original NTSC osViClock. */
}
static uint64_t guest_time(void *opaque) {
    (void)opaque;
#ifdef RECOMP_WITH_RT64
    if (wall_origin) return monotonic_ns()-wall_origin;
#endif
    return now_ns;
}
static bool submit(Runtime *rt, void *opaque, const RecompTask *task) {
    (void)rt; (void)opaque;
    require(task->words[0] == 1 || task->words[0] == 2, "Shadow Man unsupported RSP task type", task->guest_address);
    if (!reached_task) first_task = *task;
    reached_task = true;
#ifdef RECOMP_WITH_RT64
    if (task->words[0] == 2) {
        require(recomp_audio_submit(audio, task), "Shadow Man audio microcode unsupported", task->words[4]);
    } else {
        require(recomp_renderer_submit(renderer, task), "Shadow Man graphics task rejected", task->words[4]);
        graphics_tasks++;
    }
    return true;
#else
    /* A debugger boundary before backend execution. No SP/DP completion is
     * fabricated, and no microcode is claimed to have executed. */
    RecompThreadSnapshot threads[64];
    size_t count = recomp_thread_snapshots(rt, threads, 64);
    require(count < 64, "Shadow Man startup exceeded its thread budget", task->guest_address);
    for (size_t i=0; i<count; i++) if (!threads[i].current) recomp_thread_stop(rt, threads[i].object);
    recomp_thread_stop(rt, 0);
    recomp_fail("Shadow Man first-task boundary resumed", task->guest_address);
#endif
}

static void quoted(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p=(const unsigned char *)s; *p; p++) {
        if (*p=='"' || *p=='\\') fputc('\\', f);
        if (*p<32) fprintf(f, "\\u%04x", *p); else fputc(*p, f);
    }
    fputc('"', f);
}

static void failure(const RecompFailureInfo *info, void *opaque) {
    Runtime *rt = opaque;
    char path[2048], ram_path[2048];
    if (snprintf(path, sizeof(path), "%s.crash.json", report_path) >= (int)sizeof(path)
            || snprintf(ram_path, sizeof(ram_path), "%s.rdram.bin", report_path) >= (int)sizeof(ram_path)) return;
    FILE *ram = fopen(ram_path, "wb");
    if (ram) { (void)fwrite(rt->rdram, 1, rt->rdram_size, ram); fclose(ram); }
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs("{\"format_version\":1,\"failure\":{\"message\":", f);
    quoted(f, info->message);
    fprintf(f, ",\"address\":\"0x%08x\",\"requested_bytes\":%zu,\"ram_range\":%s},"
               "\"game\":{\"name\":\"Shadow Man\"},\"recent_calls\":[", info->address,
               info->requested_bytes, info->ram_range ? "true" : "false");
    unsigned retained = call_cursor < 128 ? call_cursor : 128;
    for (unsigned i = 0; i < retained; i++)
        fprintf(f, "%s\"0x%08x\"", i ? "," : "", recent_calls[(call_cursor-retained+i)%128]);
    fputs("],\"threads\":[", f);
    RecompThreadSnapshot threads[16];
    size_t count = recomp_thread_snapshots(rt, threads, 16);
    for (size_t i = 0; i < count; i++) {
        fprintf(f, "%s{\"id\":%d,\"state\":%u,\"priority\":%d,\"current\":%s,\"last_tracked_pc\":\"0x%08x\",\"gpr\":[",
                i ? "," : "", threads[i].id, (unsigned)threads[i].state, threads[i].priority,
                threads[i].current ? "true" : "false", threads[i].context.pc);
        for (unsigned j = 0; j < 32; j++) fprintf(f, "%s\"0x%016" PRIx64 "\"", j ? "," : "", threads[i].context.gpr[j]);
        fputs("]}", f);
    }
    fputs("],\"native_stack\":[", f);
    for (size_t i = 0; i < info->native_frame_count; i++) {
        Dl_info d = {0};
        (void)dladdr(info->native_frames[i], &d);
        fprintf(f, "%s{\"address\":\"0x%" PRIxPTR "\",\"module_base\":\"0x%" PRIxPTR "\",\"module\":",
                i ? "," : "", (uintptr_t)info->native_frames[i], (uintptr_t)d.dli_fbase);
        quoted(f, d.dli_fname ? d.dli_fname : ""); fputs(",\"symbol\":", f);
        quoted(f, d.dli_sname ? d.dli_sname : ""); fputc('}', f);
    }
    fputs("],\"task_words\":[", f);
    RecompTask task;
    if (recomp_task_snapshot(rt, &task))
        for (unsigned i=0; i<16; i++) fprintf(f, "%s%u", i ? "," : "", task.words[i]);
    fputs("],\"rdram\":{\"storage\":\"word-swapped\",\"path\":", f);
    quoted(f, ram_path); fputs("}}\n", f);
    fclose(f);
    fprintf(stderr, "Shadow Man crash report: %s\n", path);
}
int main(int argc, char **argv) {
#ifdef RECOMP_WITH_RT64
    if (argc != 5) { fprintf(stderr, "usage: %s NORMALIZED_ROM REPORT FRAMES RENDERER_DATA\n", argv[0]); return 2; }
    char *end;
    unsigned long frames = strtoul(argv[3], &end, 10);
    require(*argv[3] && !*end && frames <= UINT32_MAX, "Shadow Man invalid frame count", 0);
    const char *interval = getenv("RECOMP_SHADOWMAN_CAPTURE_EVERY");
    unsigned long capture_every = interval ? strtoul(interval, &end, 10) : 0;
    require(!interval || (*interval && !*end && capture_every <= UINT32_MAX), "Shadow Man invalid capture interval", 0);
#else
    if (argc != 3) { fprintf(stderr, "usage: %s NORMALIZED_ROM REPORT\n", argv[0]); return 2; }
#endif
    report_path = argv[2];
    FILE *f = fopen(argv[1], "rb");
    require(f != NULL, "cannot open Shadow Man ROM", 0);
    uint8_t *rom = malloc(BOOT_ROM_SIZE);
    require(rom != NULL, "cannot allocate Shadow Man ROM", 0);
    bool read = fread(rom, 1, BOOT_ROM_SIZE, f) == BOOT_ROM_SIZE;
    int extra = fgetc(f), closed = fclose(f);
    require(read && extra == EOF && closed == 0, "Shadow Man ROM read/size mismatch", 0);
    Runtime rt;
    require(recomp_runtime_init(&rt, BOOT_RAM_SIZE, recomp_functions, recomp_function_count,
                               recomp_boot_sections, recomp_section_count), "Shadow Man runtime init failed", 0);
    recomp_set_failure_handler(failure, &rt);
    rt.prepare_call = prepare_call;
    rt.resolve_address = shadowman_resolve_address;
    require(recomp_rom_attach(&rt, rom, BOOT_ROM_SIZE, boot_sha1), "Shadow Man ROM identity mismatch", 0);
    recomp_copy_to_ram(&rt, BOOT_INITIAL_LOAD_ADDRESS, rom + BOOT_INITIAL_ROM_OFFSET, BOOT_INITIAL_LOAD_SIZE);
    for (uint32_t a = BOOT_BSS_START; a <= BOOT_BSS_END; a += 4) recomp_store(&rt, a, 0xa5a5a5a5, 4);
    for (size_t i = 0; i < sizeof(boot_parameters)/sizeof(boot_parameters[0]); i++)
        recomp_store(&rt, boot_parameters[i][0], boot_parameters[i][1], 4);
    RecompClockConfig clock = {.ticks_per_second=BOOT_COUNTER_HZ, .now_ns=guest_time};
    RecompSchedulerConfig scheduler = {.thread_tail=0x800285d0, .run_queue=0x800285d8,
        .active_queue=0x800285dc, .running_thread=0x800285e0, .event_table=0x800503b0,
        .stop_before_entry=BOOT_STARTUP ? 0 : BOOT_MAIN_ENTRY, .fpu_fr1=true};
    RecompPlatformConfig platform = {.vi_period_ticks=781250, .vi_clock=0x800285c0,
        .app_nmi_buffer=0x8000031c};
    require(recomp_clock_init(&rt, &clock) && recomp_scheduler_init(&rt, &scheduler)
            && recomp_platform_init(&rt, &platform), "Shadow Man native services init failed", 0);
    RecompIOConfig io = {.connected_mask=1};
    RecompTaskBackend backend = {.submit=submit};
    require(recomp_io_init(&rt, &io) && recomp_tasks_init(&rt, &backend), "Shadow Man IO/task init failed", 0);
#ifdef RECOMP_WITH_RT64
    renderer = recomp_renderer_create_named(&rt, argv[4], "Shadow Man native recompilation");
    require(renderer != NULL, "Shadow Man renderer initialization failed", 0);
    RecompAudioConfig ac = {.output_enabled=true, .playback=getenv("RECOMP_AUDIO_MUTE") == NULL,
        .microcode=RECOMP_AUDIO_ABI1, .task_profile=&shadowman_audio_task_profile,
        .wav_path=getenv("RECOMP_SHADOWMAN_WAV")};
    audio = recomp_audio_create(&rt, &ac);
    require(audio != NULL, "Shadow Man audio initialization failed", 0);
    wall_origin = monotonic_ns();
    const char *replay = getenv("RECOMP_SHADOWMAN_INPUT");
    if (replay && *replay) {
        input_script = fopen(replay, "r");
        require(input_script != NULL, "Shadow Man controller replay cannot open", 0);
        read_input();
    }
#endif
    /* A synthetic host thread models IPL transfer until osStartThread. */
    recomp_thread_create(&rt, 0x807ff000, -1, BOOT_ENTRY, 0, 0x807fe000, 0);
    recomp_thread_start(&rt, 0x807ff000);
#ifdef RECOMP_WITH_RT64
    uint64_t next_present = monotonic_ns();
    while (!frames || presentations < frames) {
        RecompPad pad = {.connected=true};
        if (!recomp_renderer_input(renderer, &pad)) { window_closed = true; break; }
        if (input_script) {
            while (next_input && next_input_frame <= presentations) {
                replay_pad = pending_pad;
                read_input();
            }
            pad = replay_pad;
        }
        require(recomp_input_set(&rt, 0, &pad), "Shadow Man controller update failed", 0);
        for (unsigned batch=0; batch<32; batch++) {
            RecompRunResult run = recomp_scheduler_run(&rt, 32);
            if (run != RECOMP_RUN_BUDGET) break;
            RecompThreadSnapshot threads[32];
            size_t count = recomp_thread_snapshots(&rt, threads, 32);
            bool work = false;
            for (size_t i=0; i<count; i++)
                if (threads[i].state == RECOMP_READY && threads[i].id != 0) work = true;
            if (!work) break;
        }
        require(recomp_renderer_present(renderer), "Shadow Man presentation failed", 0);
        presentations++;
        if (capture_every && presentations%capture_every == 0 && graphics_tasks) {
            char capture[2048];
            require(snprintf(capture, sizeof(capture), "%s.frame-%06u.ppm", report_path, presentations) < (int)sizeof(capture),
                    "Shadow Man frame capture path too long", 0);
            require(recomp_renderer_capture(renderer, capture), "Shadow Man frame capture failed", 0);
        }
        next_present += 16666667;
        pace(next_present);
    }
    require(graphics_tasks || window_closed, "Shadow Man did not render a graphics task", 0);
    if (graphics_tasks) {
        char capture[2048];
        require(snprintf(capture, sizeof(capture), "%s.window.ppm", report_path) < (int)sizeof(capture),
                "Shadow Man capture path too long", 0);
        require(recomp_renderer_capture(renderer, capture), "Shadow Man capture failed", 0);
    }
    RecompRendererStats rs = recomp_renderer_stats(renderer);
    RecompAudioStats as = recomp_audio_stats(audio);
    RecompIOStats ios = recomp_io_stats(&rt);
    require(boot_bss_cleared && system_bss_cleared && tlb_installed,
            "Shadow Man rendered without verified startup", BOOT_MAIN_ENTRY);
#else
    RecompRunResult result = recomp_scheduler_run(&rt, 65536);
    if (BOOT_STARTUP) {
        for (unsigned frame = 0; frame < 600 && !reached_task; frame++) {
            now_ns += 16666667;
            result = recomp_scheduler_run(&rt, 4096);
        }
        require(reached_task, "Shadow Man did not submit an RSP task within startup budget", 0);
    }
    require(result == RECOMP_RUN_STOPPED && boot_bss_cleared && system_bss_cleared && tlb_installed,
            "Shadow Man did not reach the original application thread", BOOT_MAIN_ENTRY);
#endif
    RecompThreadSnapshot threads[16];
    size_t count = recomp_thread_snapshots(&rt, threads, 16);
    const RecompThreadSnapshot *main_thread = NULL;
    for (size_t i = 0; i < count; i++) if (threads[i].id == 0) main_thread = &threads[i];
    require(main_thread != NULL && main_thread->id == 0 && main_thread->priority == 0,
            "Shadow Man application thread ABI mismatch", BOOT_MAIN_ENTRY);
    f = fopen(report_path, "w");
    require(f != NULL, "cannot open Shadow Man report", 0);
    int written = fprintf(f, "{\"boot_bss_cleared\":true,\"system_bss_cleared\":true,\"tlb_verified\":true,"
            "\"main_state\":\"%s\",\"main_entry\":%u,\"main_thread\":%u,"
            "\"main_id\":%d,\"main_priority\":%d,\"main_stack\":%u,\"registered_functions\":%zu,"
            "\"first_task_type\":%u,\"first_task_ucode\":%u,\"gameplay_executed\":false,\"task_words\":[",
#ifdef RECOMP_WITH_RT64
            "rendered_startup",
#else
            BOOT_STARTUP ? "first_rsp_task" : "stopped_before_entry",
#endif
            BOOT_MAIN_ENTRY, main_thread->object, main_thread->id,
            main_thread->priority, (uint32_t)main_thread->context.gpr[29], recomp_function_count, first_task.words[0], first_task.words[4]);
    for (unsigned i=0; i<16; i++) fprintf(f, "%s%u", i ? "," : "", first_task.words[i]);
#ifdef RECOMP_WITH_RT64
    fprintf(f, "],\"graphics_tasks\":%u,\"presentations\":%u,\"sp_callbacks\":%" PRIu64
            ",\"dp_callbacks\":%" PRIu64 ",\"window_closed\":%s,\"audio_tasks\":%" PRIu64
            ",\"audio_frames\":%" PRIu64 ",\"audio_peak\":%u,\"audio_underruns\":%" PRIu64 ",\"audio_resets\":%" PRIu64
            ",\"controller_reads\":%" PRIu64 ",\"controller_queries\":%" PRIu64 "}\n",
            graphics_tasks, presentations, rs.sp_callbacks, rs.dp_callbacks, window_closed ? "true" : "false",
            as.tasks, as.frames, as.peak, as.queue_underruns, as.queue_resets, ios.reads_completed, ios.queries_completed);
#else
    fputs("]}\n", f);
#endif
    closed = fclose(f);
    require(written > 0 && closed == 0, "cannot write Shadow Man report", 0);
    puts(BOOT_STARTUP ? "Shadow Man submitted its original RSP tasks."
                     : "Shadow Man native bootstrap reached its original application thread.");
#ifdef RECOMP_WITH_RT64
    recomp_audio_destroy(audio);
    recomp_renderer_destroy(renderer);
    if (input_script) fclose(input_script);
#endif
    recomp_set_failure_handler(NULL, NULL);
    recomp_runtime_destroy(&rt);
    free(rom);
    return 0;
}
