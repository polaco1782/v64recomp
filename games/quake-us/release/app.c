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
#include "quake_config.h"
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
    RecompSchedulerConfig scheduler = {.thread_tail=0x800706c0,
        .run_queue=0x800706c8, .active_queue=0x800706cc, .running_thread=0x800706d0,
         .stop_before_entry=0};
    RecompPlatformConfig platform = {.vi_period_ticks=781250,
        .vi_clock=0x8006f848, .clock_rate=0x8006f840, .final_rom=0x800db0c0,
        .app_nmi_buffer=0x8000031c};
    require(recomp_clock_init(&rt, &clock) && recomp_scheduler_init(&rt, &scheduler)
            && recomp_platform_init(&rt, &platform), "Quake 64 native services init failed", 0);
    RecompIOConfig io = {.connected_mask=1, .pak_mask=1, .pak_paths={options.save}};
    RecompTaskBackend backend = {.submit=submit};
    require(recomp_io_init(&rt, &io) && recomp_tasks_init(&rt, &backend), "Quake 64 IO/task init failed", 0);
    renderer=recomp_renderer_create_named(&rt,options.data,"Quake 64");
    require(renderer!=NULL,"renderer initialization failed",0);
    RecompAudioConfig ac = {.output_enabled=true, .playback=!options.mute,
        .prebuffer_ms=160,
        .microcode=RECOMP_AUDIO_ABI1, .task_profile=&quake_audio_task_profile};
    audio=recomp_audio_create(&rt,&ac);require(audio!=NULL,"audio initialization failed",0);
    wall_origin=monotonic_ns();
    recomp_thread_create(&rt,0x803ff000,-1,BOOT_ENTRY,0,0x803fd000,0);
    recomp_thread_start(&rt,0x803ff000);
    uint64_t next_present=monotonic_ns();
    for(uint32_t frame=0;!options.frames || frame<options.frames;frame++) {
        RecompPad pad={.connected=true};
        if(!recomp_renderer_input(renderer,&pad))break;
        require(recomp_input_set(&rt,0,&pad),"controller update failed",0);
        for(unsigned batch=0;batch<32;batch++) {
            if(recomp_scheduler_run(&rt,32)!=RECOMP_RUN_BUDGET)break;
            RecompThreadSnapshot threads[32];size_t count=recomp_thread_snapshots(&rt,threads,32);
            bool work=false;
            for(size_t i=0;i<count && i<32;i++)
                if(threads[i].state==RECOMP_READY && threads[i].id!=BOOT_MAIN_ID)work=true;
            if(!work)break;
        }
        require(recomp_renderer_present(renderer),"presentation failed",0);
        next_present+=16666667;
        recomp_game_sleep(next_present);
    }
    recomp_audio_destroy(audio);recomp_renderer_destroy(renderer);
    recomp_set_failure_handler(NULL,NULL);recomp_runtime_destroy(&rt);free(rom);
    return 0;
}
