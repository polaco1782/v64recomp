#define _GNU_SOURCE
#include "runtime.h"
#include "generated.h"
#include "oot_config.h"
#include <dlfcn.h>
#include <openssl/evp.h>
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
#define LOG_OOT_CALLS 0
#if BOOT_STARTUP
#include "overlays.h"
#endif
static bool bss_cleared, main_verified;
static RecompThreadSnapshot initial_main;
static const char *report_path;
static uint32_t recent_calls[128];
static unsigned call_cursor;
static uint64_t now_ns;
static bool reached_task;
static RecompTask first_task;
static unsigned skipped_jpeg_tasks;
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
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) recomp_fail("Ocarina of Time host clock failed", 0);
    return (uint64_t)ts.tv_sec*UINT64_C(1000000000)+(uint64_t)ts.tv_nsec;
}
static void pace(Runtime *rt, uint64_t deadline) {
    while (monotonic_ns() < deadline) {
        /* VI and AI are independent of host presentation. Keep servicing them
         * during the remaining frame interval so retraces do not bunch up and
         * get discarded by the original audio manager. */
        RecompRunResult run = recomp_scheduler_run(rt, 32);
        uint64_t now = monotonic_ns();
        if (now >= deadline) break;
        if (run == RECOMP_RUN_BUDGET) continue;
        uint64_t delta = deadline-now;
        if (delta > UINT64_C(1000000)) delta=UINT64_C(1000000);
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
        require(recomp_load(rt, a, 4) == 0, "Ocarina of Time BSS was not cleared", a);
}
static void prepare_call(Runtime *rt, uint32_t address) {
    recent_calls[call_cursor++ % 128] = address;
    if (LOG_OOT_CALLS) fprintf(stderr, "Ocarina of Time call 0x%08x\n", address);
#if BOOT_STARTUP
    if (address == 0x80003040 && !rt->section_loaded[2]
            && (uint32_t)recomp_thread_context(rt, 0)->gpr[31] == 0x800adacc) {
        uint8_t *image=malloc(OOT_DD_SIZE), digest[32]; unsigned length=0;
        require(image!=NULL, "OOT DD verification allocation failed", 0);
        for(uint32_t i=0;i<OOT_DD_SIZE;i++)image[i]=(uint8_t)recomp_load(rt,0x801c7740+i,1);
        bool valid=EVP_Digest(image,OOT_DD_SIZE,digest,&length,EVP_sha256(),NULL)
            && length==32 && !memcmp(digest,oot_dd_sha256,32);
        free(image);require(valid,"OOT DD code fingerprint mismatch",0x801c7740);
        rt->section_loaded[2]=true;
    }
    if (address == 0x800adaf8 && !recomp_load(rt, 0x80121ae2, 1))
        rt->section_loaded[2]=false;
    /* The pinned main-thread loader has completed its synchronous code DMA
     * before this osGetTime call. Activate only after verifying the whole image. */
    if (address == 0x80004ad0 && !rt->section_loaded[1]
            && (uint32_t)recomp_thread_context(rt, 0)->gpr[31] == 0x800005ec) {
        uint8_t *image = malloc(OOT_CODE_SIZE), digest[32];
        unsigned length = 0;
        require(image != NULL, "OOT code verification allocation failed", 0);
        for (uint32_t i=0; i<OOT_CODE_SIZE; i++) image[i]=(uint8_t)recomp_load(rt, 0x800116e0+i, 1);
        bool valid = EVP_Digest(image, OOT_CODE_SIZE, digest, &length, EVP_sha256(), NULL)
            && length == 32 && !memcmp(digest, oot_code_sha256, 32);
        free(image);
        require(valid, "OOT decompressed code fingerprint mismatch", 0x800116e0);
        rt->section_loaded[1] = true;
    }
#endif
    if (address == BOOT_SYSTEM_ENTRY) {
        check_zero(rt, BOOT_BSS_START, BOOT_BSS_END);
        require(recomp_load(rt, BOOT_BSS_END, 4) == 0xa5a5a5a5,
                "Ocarina of Time BSS clear overran", BOOT_BSS_END);
        bss_cleared = true;
    }
    if (!main_verified && rt->scheduler) {
        RecompThreadSnapshot threads[16];
        size_t count = recomp_thread_snapshots(rt, threads, 16);
        for (size_t i = 0; i < count; i++) if (threads[i].object == BOOT_MAIN_OBJECT) {
            initial_main = threads[i];
            require(initial_main.id == BOOT_MAIN_ID && initial_main.priority == BOOT_MAIN_PRIORITY
                    && initial_main.context.pc == BOOT_MAIN_ENTRY
                    && (uint32_t)initial_main.context.gpr[29] == BOOT_MAIN_STACK-16,
                    "Ocarina of Time initial application thread ABI mismatch", BOOT_MAIN_ENTRY);
            main_verified = true;
        }
    }

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
    if (task->words[0] == 4) {
        require(task->words[4] == 0x800e7200 && !task->resumed,
                "OOT unrecognized JPEG task", task->words[4]);
        skipped_jpeg_tasks++;
        return recomp_task_complete(rt, task->token, RECOMP_SP_DONE);
    }
    require(task->words[0] == 1 || task->words[0] == 2, "Ocarina of Time unsupported RSP task type", task->guest_address);
    if (!reached_task) first_task = *task;
    reached_task = true;
#ifdef RECOMP_WITH_RT64
    if (task->words[0] == 2) {
        require(recomp_audio_submit(audio, task), "Ocarina of Time audio microcode unsupported", task->words[4]);
    } else {
        require(recomp_renderer_submit(renderer, task), "Ocarina of Time graphics task rejected", task->words[4]);
        graphics_tasks++;
    }
    return true;
#else
    /* A debugger boundary before backend execution. No SP/DP completion is
     * fabricated, and no microcode is claimed to have executed. */
    RecompThreadSnapshot threads[64];
    size_t count = recomp_thread_snapshots(rt, threads, 64);
    require(count < 64, "Ocarina of Time startup exceeded its thread budget", task->guest_address);
    for (size_t i=0; i<count; i++) if (!threads[i].current) recomp_thread_stop(rt, threads[i].object);
    recomp_thread_stop(rt, 0);
    recomp_fail("Ocarina of Time first-task boundary resumed", task->guest_address);
#endif
}

/* The pinned CIC task only stores vector state in DMEM and signals SP.
 * Its CPU-visible result words are the zero data/yield sizes in the OSTask.
 * Native graphics does not retain an RSP register file or run IPL security code. */
void hook_oot_cic_boot_task(Runtime *rt, MipsContext *ctx) {
    recomp_store(rt, 0x80008ef8, 0, 4);
    recomp_store(rt, 0x80008efc, 0, 4);
    mips_set(ctx, 2, 0);
}

/* IPL does not return to the entry stub after handing control to libultra. */
void hook_oot_start_thread(Runtime *rt, MipsContext *ctx) {
    hook_os_start_thread(rt, ctx);
    if (recomp_thread_id(rt, 0) == -1) recomp_thread_stop(rt, 0);
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
               "\"game\":{\"name\":\"Ocarina of Time\"},\"recent_calls\":[", info->address,
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
    fprintf(stderr, "Ocarina of Time crash report: %s\n", path);
}
int main(int argc, char **argv) {
#ifdef RECOMP_WITH_RT64
    if (argc != 5) { fprintf(stderr, "usage: %s NORMALIZED_ROM REPORT FRAMES RENDERER_DATA\n", argv[0]); return 2; }
    char *end;
    unsigned long frames = strtoul(argv[3], &end, 10);
    require(*argv[3] && !*end && frames <= UINT32_MAX, "Ocarina of Time invalid frame count", 0);
    const char *interval = getenv("RECOMP_OOT_CAPTURE_EVERY");
    unsigned long capture_every = interval ? strtoul(interval, &end, 10) : 0;
    require(!interval || (*interval && !*end && capture_every <= UINT32_MAX), "Ocarina of Time invalid capture interval", 0);
#else
    if (argc != 3) { fprintf(stderr, "usage: %s NORMALIZED_ROM REPORT\n", argv[0]); return 2; }
#endif
    report_path = argv[2];
    FILE *f = fopen(argv[1], "rb");
    require(f != NULL, "cannot open Ocarina of Time ROM", 0);
    uint8_t *rom = malloc(BOOT_ROM_SIZE);
    require(rom != NULL, "cannot allocate Ocarina of Time ROM", 0);
    bool read = fread(rom, 1, BOOT_ROM_SIZE, f) == BOOT_ROM_SIZE;
    int extra = fgetc(f), closed = fclose(f);
    require(read && extra == EOF && closed == 0, "Ocarina of Time ROM read/size mismatch", 0);
    Runtime rt;
    require(recomp_runtime_init(&rt, BOOT_RAM_SIZE, recomp_functions, recomp_function_count,
                               recomp_boot_sections, recomp_section_count), "Ocarina of Time runtime init failed", 0);
    recomp_set_failure_handler(failure, &rt);
    rt.prepare_call = prepare_call;
#if BOOT_STARTUP
    rt.canonical_function_address=oot_canonical;
    rt.relocated_address=oot_relocated;
#endif
    require(recomp_rom_attach(&rt, rom, BOOT_ROM_SIZE, boot_sha1), "Ocarina of Time ROM identity mismatch", 0);
    recomp_copy_to_ram(&rt, BOOT_INITIAL_LOAD_ADDRESS, rom + BOOT_INITIAL_ROM_OFFSET, BOOT_INITIAL_LOAD_SIZE);
    for (uint32_t a = BOOT_BSS_START; a <= BOOT_BSS_END; a += 4) recomp_store(&rt, a, 0xa5a5a5a5, 4);
    for (size_t i = 0; i < sizeof(boot_parameters)/sizeof(boot_parameters[0]); i++)
        recomp_store(&rt, boot_parameters[i][0], boot_parameters[i][1], 4);
    RecompClockConfig clock = {.ticks_per_second=BOOT_COUNTER_HZ, .now_ns=guest_time};
    RecompSchedulerConfig scheduler = {.thread_tail=BOOT_THREAD_TAIL,
        .run_queue=BOOT_RUN_QUEUE, .active_queue=BOOT_ACTIVE_QUEUE, .running_thread=BOOT_RUNNING_THREAD,
        .idle_thread=BOOT_MAIN_OBJECT,
        .stop_before_entry=BOOT_STARTUP ? 0 : BOOT_MAIN_ENTRY};
    char save_path[2048];
    require(snprintf(save_path,sizeof(save_path),"%s.sram",argv[1])<(int)sizeof(save_path), "OOT save path too long", 0);
    RecompPlatformConfig platform = {.vi_period_ticks=781250,
        .vi_clock=BOOT_VI_CLOCK, .clock_rate=BOOT_CLOCK_RATE,
        .sram_size=32768, .sram_path=save_path,
        .cart_handle=BOOT_CART_HANDLE, .drive_handle=BOOT_DRIVE_HANDLE,
        .app_nmi_buffer=0x8000031c};
    require(recomp_clock_init(&rt, &clock) && recomp_scheduler_init(&rt, &scheduler)
            && recomp_platform_init(&rt, &platform), "Ocarina of Time native services init failed", 0);
    /* The priority-15 pad thread continuously reads. A 1 ms serial transfer
     * allows priority-10 game code to run between controller reads. */
    RecompIOConfig io = {.connected_mask=1, .si_transfer_ticks=BOOT_COUNTER_HZ/1000};
    RecompTaskBackend backend = {.submit=submit, .additional_task_types=1u<<4};
    require(recomp_io_init(&rt, &io) && recomp_tasks_init(&rt, &backend), "Ocarina of Time IO/task init failed", 0);
#ifdef RECOMP_WITH_RT64
    renderer = recomp_renderer_create_named(&rt, argv[4], "Ocarina of Time native recompilation");
    require(renderer != NULL, "Ocarina of Time renderer initialization failed", 0);
    RecompAudioConfig ac = {.output_enabled=true, .playback=getenv("RECOMP_AUDIO_MUTE") == NULL,
        .prebuffer_ms=160,
        .microcode=RECOMP_AUDIO_NEAD_OOT, .task_profile=&oot_audio_task_profile,
        .trace_path=getenv("RECOMP_OOT_AUDIO_TRACE"),
        .wav_path=getenv("RECOMP_OOT_WAV")};
    audio = recomp_audio_create(&rt, &ac);
    require(audio != NULL, "Ocarina of Time audio initialization failed", 0);
    wall_origin = monotonic_ns();
    const char *replay = getenv("RECOMP_OOT_INPUT");
    if (replay && *replay) {
        input_script = fopen(replay, "r");
        require(input_script != NULL, "Ocarina of Time controller replay cannot open", 0);
        read_input();
    }
#endif
    /* A synthetic host thread models IPL transfer until osStartThread. */
    recomp_thread_create(&rt, 0x803ff000, -1, BOOT_ENTRY, 0, 0x803fd000, 0);
    recomp_thread_start(&rt, 0x803ff000);
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
        require(recomp_input_set(&rt, 0, &pad), "Ocarina of Time controller update failed", 0);
        for (unsigned batch=0; batch<32; batch++) {
            RecompRunResult run = recomp_scheduler_run(&rt, 32);
            if (run != RECOMP_RUN_BUDGET) break;
            RecompThreadSnapshot threads[32];
            size_t count = recomp_thread_snapshots(&rt, threads, 32);
            bool work = false;
            for (size_t i=0; i<count; i++)
                if (threads[i].state == RECOMP_READY && threads[i].id != BOOT_MAIN_ID) work = true;
            if (!work) break;
        }
        require(recomp_renderer_present(renderer), "Ocarina of Time presentation failed", 0);
        presentations++;
        if (capture_every && presentations%capture_every == 0 && graphics_tasks) {
            char capture[2048];
            require(snprintf(capture, sizeof(capture), "%s.frame-%06u.ppm", report_path, presentations) < (int)sizeof(capture),
                    "Ocarina of Time frame capture path too long", 0);
            require(recomp_renderer_capture(renderer, capture), "Ocarina of Time frame capture failed", 0);
        }
        next_present += 16666667;
        pace(&rt, next_present);
    }
    require(graphics_tasks || window_closed, "Ocarina of Time did not render a graphics task", 0);
    if (graphics_tasks) {
        char capture[2048];
        require(snprintf(capture, sizeof(capture), "%s.window.ppm", report_path) < (int)sizeof(capture),
                "Ocarina of Time capture path too long", 0);
        require(recomp_renderer_capture(renderer, capture), "Ocarina of Time capture failed", 0);
    }
    RecompRendererStats rs = recomp_renderer_stats(renderer);
    RecompAudioStats as = recomp_audio_stats(audio);
    RecompIOStats ios = recomp_io_stats(&rt);
    require(bss_cleared,
            "Ocarina of Time rendered without verified startup", BOOT_MAIN_ENTRY);
#else
    RecompRunResult result = recomp_scheduler_run(&rt, BOOT_STARTUP ? 32 : 65536);
    if (BOOT_STARTUP) {
        for (unsigned frame = 0; frame < 600 && !reached_task; frame++) {
            now_ns += 16666667;
            result = recomp_scheduler_run(&rt, 32);
        }
        require(reached_task, "Ocarina of Time did not submit an RSP task within startup budget", 0);
    }
    require(result == RECOMP_RUN_STOPPED && bss_cleared,
            "Ocarina of Time did not reach the original application thread", BOOT_MAIN_ENTRY);
#endif
    RecompThreadSnapshot threads[16];
    size_t count = recomp_thread_snapshots(&rt, threads, 16);
    const RecompThreadSnapshot *main_thread = NULL;
    for (size_t i = 0; i < count; i++) if (threads[i].object == BOOT_MAIN_OBJECT) main_thread = &threads[i];
    require(main_verified && main_thread != NULL && main_thread->id == BOOT_MAIN_ID,
            "Ocarina of Time application thread ABI mismatch", BOOT_MAIN_ENTRY);
    f = fopen(report_path, "w");
    require(f != NULL, "cannot open Ocarina of Time report", 0);
    fprintf(f, "{\"skipped_jpeg_tasks\":%u,", skipped_jpeg_tasks);
    int written = fprintf(f, "\"bss_cleared\":true,\"bss_guard_verified\":true,"
            "\"main_state\":\"%s\",\"main_entry\":%u,\"main_thread\":%u,"
            "\"main_id\":%d,\"main_priority\":%d,\"main_stack\":%u,\"registered_functions\":%zu,"
            "\"first_task_type\":%u,\"first_task_ucode\":%u,\"task_words\":[",
#ifdef RECOMP_WITH_RT64
            "rendered_startup",
#else
            BOOT_STARTUP ? "first_rsp_task" : "stopped_before_entry",
#endif
            BOOT_MAIN_ENTRY, main_thread->object, main_thread->id,
            initial_main.priority, (uint32_t)initial_main.context.gpr[29], recomp_function_count, first_task.words[0], first_task.words[4]);
    for (unsigned i=0; i<16; i++) fprintf(f, "%s%u", i ? "," : "", first_task.words[i]);
#ifdef RECOMP_WITH_RT64
    fprintf(f, "],\"graphics_tasks\":%u,\"presentations\":%u,\"sp_callbacks\":%" PRIu64
            ",\"dp_callbacks\":%" PRIu64 ",\"graphics_tasks_without_dp\":%" PRIu64 ",\"window_closed\":%s,\"audio_tasks\":%" PRIu64
            ",\"audio_frames\":%" PRIu64 ",\"audio_peak\":%u,\"audio_underruns\":%" PRIu64 ",\"audio_resets\":%" PRIu64
            ",\"controller_reads\":%" PRIu64 ",\"controller_queries\":%" PRIu64,
            graphics_tasks, presentations, rs.sp_callbacks, rs.dp_callbacks, rs.graphics_tasks_without_dp, window_closed ? "true" : "false",
            as.tasks, as.frames, as.peak, as.queue_underruns, as.queue_resets, ios.reads_completed, ios.queries_completed);
    fputs("}\n", f);
#else
    fputs("]}\n", f);
#endif
    closed = fclose(f);
    require(written > 0 && closed == 0, "cannot write Ocarina of Time report", 0);
    puts(BOOT_STARTUP ? "Ocarina of Time submitted its original RSP tasks."
                     : "Ocarina of Time native bootstrap reached its original application thread.");
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
