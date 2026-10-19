#define _POSIX_C_SOURCE 200809L
#include "perfectdark.h"
#include "generated.h"
#include "renderer.h"
#include "recomp_audio.h"
#include "audio.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "perfectdark_config.h"
#define require recomp_game_require
static void initialize_services(Runtime *rt);
static void verify(Runtime *,uint32_t,size_t,const uint8_t[32]);
static void prepare_call(Runtime *rt,uint32_t address) {
    if(address==0x70005b60 && !rt->scheduler)initialize_services(rt);
    if(address==0x7004e1b0 && !rt->section_loaded[4]) {
        const MipsContext *ctx=recomp_thread_context(rt,0);
        if((uint32_t)ctx->gpr[6]==0x7f1a0000) {
            uint32_t base=((uint32_t)ctx->gpr[7]-0x1a0000)|0x80000000;
            verify(rt,base,PD_GAME_SIZE,pd_game_sha256);
            rt->section_loaded[4]=true;
        }
    }
}
static void verify(Runtime *rt,uint32_t address,size_t size,const uint8_t expected[32]) {
    uint8_t *raw=malloc(size), digest[32];unsigned length=0;
    require(raw!=NULL,"Perfect Dark verification allocation failed",address);
    for(size_t i=0;i<size;i++)raw[i]=(uint8_t)recomp_load(rt,address+(uint32_t)i,1);
    bool valid=EVP_Digest(raw,size,digest,&length,EVP_sha256(),NULL) && length==32 && !memcmp(digest,expected,32);
    free(raw);require(valid,"Perfect Dark loaded image fingerprint mismatch",address);
}
void hook_pd_boot_tlb(Runtime *rt,MipsContext *ctx) {
    recomp_map_pages(rt,0,0x7fe000,0x70000000,0,0x400000);
    recomp_lookup(rt,0x700016cc)(rt,ctx);
}
void hook_pd_unmap_range(Runtime *rt,MipsContext *ctx) {
    unsigned first=(uint32_t)ctx->gpr[4],last=(uint32_t)ctx->gpr[5];
    require(first<=last && last<32,"Perfect Dark invalid TLB range",first);
    for(unsigned i=first;i<=last;i++)rt->page_mappings[i]=(RecompPageMapping){0};
}
void hook_pd_boot_rom_read(Runtime *rt,MipsContext *ctx) {
    /* mtx_get_obfuscated_rom_base serializes a raw PI read of the immutable
     * boot ROM word. Native DMA is already serialized by the runtime. */
    uint32_t base=(uint32_t)recomp_load(rt,0x80000308,4);
    uint32_t address=(base|0xb764b4fd)^0x0764bea1;
    require(address>=0xb0000000 && address-0xb0000000<=rt->rom_size-4,
        "Perfect Dark boot ROM read outside cartridge",address);
    const uint8_t *p=rt->rom+address-0xb0000000;
    uint32_t value=(uint32_t)p[0]<<24|(uint32_t)p[1]<<16|(uint32_t)p[2]<<8|p[3];
    mips_set(ctx,2,mips_s32(value));
}
void hook_pd_enter_inflate(Runtime *rt,MipsContext *ctx) {
    if(!rt->section_loaded[2]) {
        verify(rt,PD_INFLATE_ADDRESS,PD_INFLATE_SIZE,pd_inflate_sha256);
        rt->section_loaded[2]=true;
    }
    mips_set(ctx,7,0x7020126c);
    recomp_lookup(rt,0x7020126c)(rt,ctx);
}
static uint64_t guest_time(void *opaque) {
    PerfectDark *game=opaque;
    return game->wall_origin?game->now_ns+recomp_game_monotonic()-game->wall_origin:game->now_ns;
}
static bool submit(Runtime *rt,void *opaque,const RecompTask *task) {
    (void)rt;PerfectDark *game=opaque;
    if(task->words[0]==2) {
        game->audio_tasks++;
        require(recomp_audio_submit(game->audio,task),"Perfect Dark unsupported audio task",task->words[4]);
    } else {
        require(task->words[0]==1,"Perfect Dark unsupported RSP task",task->words[0]);
        game->graphics_tasks++;
        require(recomp_renderer_submit(game->renderer,task),"Perfect Dark graphics task rejected",task->words[4]);
    }
    return true;
}
static void initialize_services(Runtime *rt) {
    /* Runtime is the first member of the owning game state. */
    PerfectDark *game=(PerfectDark *)rt;
    verify(rt,PD_LIB_ADDRESS,PD_LIB_SIZE,pd_lib_sha256);
    verify(rt,PD_DATA_ADDRESS,PD_DATA_SIZE,pd_data_sha256);
    rt->section_loaded[3]=true;
    RecompClockConfig clock={.ticks_per_second=BOOT_COUNTER_HZ,.now_ns=guest_time,.userdata=game,.timer_sentinel=0x8009c760};
    RecompSchedulerConfig scheduler={.thread_tail=0x80060930,.run_queue=0x80060938,
        .active_queue=0x8006093c,.running_thread=0x80060940,.idle_thread=0x8008d4a0};
    RecompPlatformConfig platform={.vi_period_ticks=781250,.clock_rate=0x8005cf60,
        .vi_clock=0x8005cf68,.final_rom=0x80090a60,.timer_list=0x800608a0,
        .current_time=0x8009c780,.base_counter=0x8009c788,.vi_intr_count=0x8009c78c,.app_nmi_buffer=0x8000031c};
    require(recomp_clock_init(rt,&clock) && recomp_scheduler_init(rt,&scheduler)
        && recomp_platform_init(rt,&platform),"Perfect Dark services init failed",0);
    RecompIOConfig io={.connected_mask=1,.eeprom_size=2048,.eeprom_path=game->options.save,.si_transfer_ticks=1000};
    RecompTaskBackend backend={.userdata=game,.submit=submit};
    require(recomp_io_init(rt,&io) && recomp_tasks_init(rt,&backend),"Perfect Dark IO/task init failed",0);
    RecompAudioConfig ac={.output_enabled=true,.playback=!game->options.mute,.prebuffer_ms=160,
        .microcode=RECOMP_AUDIO_NAUDIO_MP3,.task_profile=&perfectdark_audio_task_profile};
    game->audio=recomp_audio_create(rt,&ac);require(game->audio!=NULL,"Perfect Dark audio initialization failed",0);
}
void perfectdark_start(PerfectDark *game,const RecompGameOptions *options) {
    *game=(PerfectDark){.options=*options};
    Runtime *rt=&game->runtime;
    require(recomp_runtime_init(rt,BOOT_RAM_SIZE,recomp_functions,recomp_function_count,
        recomp_boot_sections,recomp_section_count),"Perfect Dark runtime init failed",0);
    rt->prepare_call=prepare_call;
    game->rom=recomp_game_rom(rt,options,BOOT_ROM_SIZE,boot_sha1);
    recomp_copy_to_ram(rt,0x80000000,game->rom,0x1000);
    recomp_copy_to_ram(rt,BOOT_INITIAL_LOAD_ADDRESS,game->rom+BOOT_INITIAL_ROM_OFFSET,BOOT_INITIAL_LOAD_SIZE);
    for(size_t i=0;i<sizeof(boot_parameters)/sizeof(boot_parameters[0]);i++)
        recomp_store(rt,boot_parameters[i][0],boot_parameters[i][1],4);
    game->renderer=recomp_renderer_create_named(rt,options->data,"Perfect Dark");
    require(game->renderer!=NULL,"Perfect Dark renderer initialization failed",0);
    MipsContext boot={.fcr31=0x01000800};boot.gpr[31]=mips_s32(0xf0000000);
    recomp_lookup(rt,BOOT_ENTRY)(rt,&boot);
    game->start_ns=recomp_game_monotonic();
    game->next_ns=game->start_ns;
}
void perfectdark_step(PerfectDark *game,const RecompPad *pad) {
    Runtime *rt=&game->runtime;
    require(recomp_input_set(rt,0,pad),"Perfect Dark controller update failed",0);
    for(unsigned i=0;i<256;i++)if(recomp_scheduler_run(rt,16)!=RECOMP_RUN_BUDGET)break;
    require(recomp_renderer_present(game->renderer),"Perfect Dark presentation failed",0);
    if(!game->wall_origin) {
        game->now_ns+=16666667;
        if(game->graphics_tasks)game->wall_origin=recomp_game_monotonic();
    }
    game->next_ns+=16666667;
    /* Keep the deterministic boot clock until the first graphics handoff. */
    if(game->wall_origin)recomp_game_pace(rt,game->next_ns);
    else recomp_game_sleep(game->next_ns);
}
void perfectdark_stop(PerfectDark *game) {
    fprintf(stderr,"Perfect Dark: %u graphics tasks, %u audio tasks\n",game->graphics_tasks,game->audio_tasks);
    recomp_audio_destroy(game->audio);recomp_renderer_destroy(game->renderer);
    recomp_runtime_destroy(&game->runtime);free(game->rom);
}
