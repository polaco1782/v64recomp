#define _POSIX_C_SOURCE 200809L
#include "hexen.h"
#include "graphics.h"
#include "audio.h"
#include "generated.h"
#include "hexen_config.h"
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define require recomp_game_require
static Hexen *game;

static void check_image(Runtime *rt, uint32_t address, uint32_t size, const uint8_t expected[32]) {
    uint8_t *raw = malloc(size), digest[32];
    require(raw != NULL, "Hexen image check allocation failed", address);
    for (uint32_t i=0;i<size;i++) raw[i]=(uint8_t)recomp_load(rt,address+i,1);
    SHA256(raw, size, digest);
    free(raw);
    require(!memcmp(digest, expected, 32), "Hexen guest decoded image mismatch", address);
}
static void prepare_call(Runtime *rt, uint32_t address) {
#ifdef RECOMP_DEBUG
    if (getenv("RECOMP_TRACE_CALLS")) fprintf(stderr, "Hexen call 0x%08x\n", address);
#endif
    if (address == BOOT_SYSTEM_ENTRY) {
        for (uint32_t a=BOOT_BSS_START; a<BOOT_BSS_END; a+=4)
            require(recomp_load(rt,a,4)==0,"Hexen boot BSS clear failed",a);
        game->bss_verified=true;
    }
    if (address == BOOT_MAIN_HANDOFF_CALL) {
        /* The first real thread has taken the token. Join IPL before upper
         * RAM becomes guest graphics/task storage. */
        RecompThreadSnapshot threads[16];
        size_t count=recomp_thread_snapshots(rt,threads,16);
        for(size_t i=0;i<count && i<16;i++)
            if(threads[i].current && threads[i].object==BOOT_MAIN_OBJECT)
                recomp_thread_destroy(rt, 0x803ff000);
    }
    if (address == 0x8004d078 && !game->program_verified) {
        /* LZH tree reconstruction calls the game's memmove after its bytes
         * have been decoded. The section is partially resident until return;
         * guard every entry below, retaining the core loaded-section checks. */
        rt->section_loaded[BOOT_GAME_SECTION]=true;
    }
    if (!game->program_verified && address >= BOOT_DECODED_ADDRESS
            && address < BOOT_DECODED_ADDRESS+BOOT_DECODED_SIZE) {
        if (address == 0x800dc9b0) {
            /* Full helper body is checked against the prepared ROM image. */
            if (!game->helper_verified) {
                check_image(rt,address,0x78,memmove_sha256);
                game->helper_verified=true;
            }
        } else {
            check_image(rt,BOOT_DECODED_ADDRESS,BOOT_DECODED_SIZE,decoded_sha256);
            game->program_verified=true;
        }
    }
}
static uint64_t guest_time(void *opaque) {
    Hexen *g=opaque;
    return recomp_game_monotonic()-g->wall_origin;
}
static bool submit(Runtime *rt, void *opaque, const RecompTask *task) {
    (void)rt;
    Hexen *g=opaque;
#ifdef RECOMP_DEBUG
    static bool reported[3];
    unsigned type=task->words[0];
    if(type<3 && !reported[type]) {
        reported[type]=true;
        fprintf(stderr,"Hexen task %u:",type);
        for(unsigned i=0;i<16;i++)fprintf(stderr," %08x",task->words[i]);
        fputc('\n',stderr);
    }
#endif
    require(task->words[0]==1 || task->words[0]==2,"Hexen unsupported RSP task type",task->guest_address);
    RecompTask normalized=*task;
    if(task->words[0]==1) {
        require(task->words[13]<=UINT32_MAX/sizeof(uint64_t),"Hexen display-list command count overflow",task->guest_address);
        normalized.words[13]*=sizeof(uint64_t);
    }
    bool ok=task->words[0]==2 ? recomp_audio_submit(g->audio,task) : recomp_renderer_submit(g->renderer,&normalized);
    require(ok,"Hexen RSP task rejected",task->words[4]);
    return true;
}
void hexen_init(Hexen *g, const RecompGameOptions *options, const char *wav) {
    memset(g,0,sizeof(*g));game=g;
    Runtime *rt=&g->runtime;
    require(recomp_runtime_init(rt,BOOT_RAM_SIZE,recomp_functions,recomp_function_count,
        recomp_boot_sections,recomp_section_count),"Hexen runtime initialization failed",0);
    g->rom=recomp_game_rom(rt,options,BOOT_ROM_SIZE,boot_sha1);
    rt->prepare_call=prepare_call;
    recomp_copy_to_ram(rt,BOOT_INITIAL_LOAD_ADDRESS,g->rom+BOOT_INITIAL_ROM_OFFSET,BOOT_INITIAL_LOAD_SIZE);
    for(size_t i=0;i<sizeof(boot_parameters)/sizeof(boot_parameters[0]);i++)
        recomp_store(rt,boot_parameters[i][0],boot_parameters[i][1],4);
    g->wall_origin=recomp_game_monotonic();
    RecompClockConfig clock={.ticks_per_second=BOOT_COUNTER_HZ,.now_ns=guest_time,.userdata=g};
    RecompSchedulerConfig scheduler=hexen_scheduler_config;
    scheduler.idle_thread=BOOT_MAIN_OBJECT;
    static const RecompBootLoad loads[]={
        {BOOT_SUPPORT_SECTION,BOOT_SUPPORT_ADDRESS,BOOT_SUPPORT_ROM_OFFSET,BOOT_SUPPORT_SIZE}
    };
    RecompPlatformConfig platform=hexen_platform_config;
    platform.vi_period_ticks=781250;
    platform.code_loads=loads;platform.code_load_count=1;
    require(recomp_clock_init(rt,&clock) && recomp_scheduler_init(rt,&scheduler)
        && recomp_platform_init(rt,&platform),"Hexen native services initialization failed",0);
    RecompIOConfig io={.connected_mask=1,.pak_mask=1,.pak_paths={options->save}};
    RecompTaskBackend backend={.submit=submit,.userdata=g};
    require(recomp_io_init(rt,&io) && recomp_tasks_init(rt,&backend),"Hexen IO/tasks initialization failed",0);
    g->renderer=recomp_renderer_create_named(rt,options->data,"Hexen");
    require(g->renderer!=NULL,"Hexen renderer initialization failed",0);
    recomp_renderer_set_microcode_hook(g->renderer,hexen_bind_microcode);
    RecompAudioConfig ac={.output_enabled=true,.playback=!options->mute,
        .prebuffer_ms=160,.microcode=RECOMP_AUDIO_ABI1,
        .task_profile=&hexen_audio_task_profile,.wav_path=wav};
    g->audio=recomp_audio_create(rt,&ac);
    require(g->audio!=NULL,"Hexen audio initialization failed",0);
    recomp_thread_create(rt,0x803ff000,-1,BOOT_ENTRY,0,0x803fd000,0);
    recomp_thread_start(rt,0x803ff000);
}
void hexen_step(Hexen *g, const RecompPad *pad) {
    Runtime *rt=&g->runtime;
    if(pad)require(recomp_input_set(rt,0,pad),"Hexen controller update failed",0);
    for(unsigned batch=0;batch<16;batch++) {
        if(recomp_scheduler_run(rt,16)!=RECOMP_RUN_BUDGET)break;
        RecompThreadSnapshot threads[32];size_t count=recomp_thread_snapshots(rt,threads,32);
        bool work=false;
        for(size_t i=0;i<count && i<32;i++)
            if(threads[i].state==RECOMP_READY && threads[i].object!=BOOT_MAIN_OBJECT)work=true;
        if(!work)break;
    }
}
void hexen_pace(Hexen *g, uint64_t deadline) {
    for(;;) {
        uint64_t now=recomp_game_monotonic();
        if(now>=deadline)return;
        uint64_t wake=now+1000000;
        recomp_game_sleep(wake<deadline?wake:deadline);
        hexen_step(g,NULL);
    }
}
void hexen_destroy(Hexen *g) {
    recomp_audio_destroy(g->audio);
    recomp_renderer_destroy(g->renderer);
    recomp_runtime_destroy(&g->runtime);
    free(g->rom);game=NULL;
}
