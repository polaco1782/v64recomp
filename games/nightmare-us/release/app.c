#define _POSIX_C_SOURCE 200809L
#include "game.h"
#include "generated.h"
#include "renderer.h"
#include "audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define require recomp_game_require
#define monotonic_ns recomp_game_monotonic
static RecompRenderer *renderer;
static RecompAudio *audio;
static uint64_t now_ns, wall_origin;
#include "nightmare_config.h"
#include "hooks.h"
static void drain(Runtime *rt) {
    for (unsigned batch=0; batch<128; batch++) {
        RecompRunResult run = recomp_scheduler_run(rt, 8);
        if (run != RECOMP_RUN_BUDGET) break;
        RecompThreadSnapshot threads[32];
        size_t count = recomp_thread_snapshots(rt, threads, 32);
        bool work = false;
        for (size_t i=0; i<count; i++)
            if (threads[i].state == RECOMP_READY && threads[i].id != BOOT_MAIN_ID)
                work = true;
        if (!work) break;
    }
}
static void pace(Runtime *rt, uint64_t deadline) {
    for (;;) {
        uint64_t now = monotonic_ns();
        if (now >= deadline) return;
        uint64_t wake = deadline;
        uint64_t event = recomp_platform_next_deadline(rt);
        uint64_t timer = recomp_clock_next_deadline(rt);
        if (timer < event) event = timer;
        if (event != UINT64_MAX) {
            uint64_t ticks = recomp_count64(rt), hz = recomp_clock_frequency(rt);
            if (event <= ticks) wake = now;
            else if (event-ticks < hz) {
                uint64_t ns = ((event-ticks)*UINT64_C(1000000000)+hz-1)/hz;
                if (ns < deadline-now) wake = now+ns;
            }
        }
        /* SI and asynchronous RSP workers also need service between VIs.
         * Keep the owner's idle polling bounded to one millisecond. */
        if (wake-now > UINT64_C(1000000)) wake = now+UINT64_C(1000000);
        if (wake > now) {
            uint64_t delta = wake-now;
            struct timespec ts = {(time_t)(delta/1000000000), (long)(delta%1000000000)};
            nanosleep(&ts, NULL);
        }
        drain(rt);
    }
}
static uint64_t guest_time(void *opaque) {
    (void)opaque;
    return wall_origin ? monotonic_ns()-wall_origin : now_ns;
}
static bool submit(Runtime *rt, void *opaque, const RecompTask *task) {
    (void)rt; (void)opaque;
    require(task->words[0]==1 || task->words[0]==2,"unsupported RSP task type",task->guest_address);
    bool ok=task->words[0]==2 ? recomp_audio_submit(audio,task) : recomp_renderer_submit(renderer,task);
    require(ok,"game task rejected",task->words[4]);
    return true;
}
int main(int argc,char **argv) {
    RecompGameOptions options;int parsed=recomp_game_options(argc,argv,&options);
    if(parsed)return parsed<0?2:0;
    Runtime rt;
    require(recomp_runtime_init(&rt,BOOT_RAM_SIZE,recomp_functions,recomp_function_count,
        recomp_boot_sections,recomp_section_count),"runtime init failed",0);
    uint8_t *rom=recomp_game_rom(&rt,&options,BOOT_ROM_SIZE,boot_sha1);
    recomp_copy_to_ram(&rt,BOOT_INITIAL_LOAD_ADDRESS,rom+BOOT_INITIAL_ROM_OFFSET,BOOT_INITIAL_LOAD_SIZE);
    for(size_t i=0;i<sizeof(boot_parameters)/sizeof(boot_parameters[0]);i++)
        recomp_store(&rt,boot_parameters[i][0],boot_parameters[i][1],4);
    RecompClockConfig clock = {.ticks_per_second=BOOT_COUNTER_HZ, .now_ns=guest_time};
    RecompSchedulerConfig scheduler = {.thread_tail=BOOT_THREAD_TAIL,
        .run_queue=BOOT_RUN_QUEUE, .active_queue=BOOT_ACTIVE_QUEUE, .running_thread=BOOT_RUNNING_THREAD,
        .idle_thread=BOOT_MAIN_OBJECT,
         .stop_before_entry=0};
    RecompPlatformConfig platform = {.vi_period_ticks=781250,
        .vi_clock=BOOT_VI_CLOCK, .clock_rate=BOOT_CLOCK_RATE,
        .app_nmi_buffer=0x8000031c};
    require(recomp_clock_init(&rt, &clock) && recomp_scheduler_init(&rt, &scheduler)
            && recomp_platform_init(&rt, &platform), "Nightmare Creatures native services init failed", 0);
    /* The priority-15 pad thread continuously reads. A 1 ms serial transfer
     * allows priority-10 game code to run between controller reads. */
    RecompIOConfig io = {.connected_mask=1, .si_transfer_ticks=BOOT_COUNTER_HZ/1000};
    RecompTaskBackend backend = {.submit=submit};
    require(recomp_io_init(&rt, &io) && recomp_tasks_init(&rt, &backend), "Nightmare Creatures IO/task init failed", 0);
    renderer=recomp_renderer_create_named(&rt,options.data,"Nightmare Creatures");
    require(renderer!=NULL,"renderer initialization failed",0);
    RecompAudioConfig ac = {.output_enabled=true, .playback=!options.mute,
        .prebuffer_ms=160,
        .microcode=RECOMP_AUDIO_ABI1, .task_profile=&nightmare_audio_task_profile};
    audio=recomp_audio_create(&rt,&ac);require(audio!=NULL,"audio initialization failed",0);
    wall_origin=monotonic_ns();
    recomp_thread_create(&rt,0x803ff000,-1,BOOT_ENTRY,0,0x803fd000,0);
    recomp_thread_start(&rt,0x803ff000);
    uint64_t next_present=monotonic_ns();
    for(uint32_t frame=0;!options.frames || frame<options.frames;frame++) {
        RecompPad pad={.connected=true};
        if(!recomp_renderer_input(renderer,&pad))break;
        require(recomp_input_set(&rt,0,&pad),"controller update failed",0);
        drain(&rt);
        require(recomp_renderer_present(renderer),"presentation failed",0);
        next_present+=16666667;
        pace(&rt,next_present);
    }
    recomp_audio_destroy(audio);recomp_renderer_destroy(renderer);
    recomp_set_failure_handler(NULL,NULL);recomp_runtime_destroy(&rt);free(rom);
    return 0;
}
