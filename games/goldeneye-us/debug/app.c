#define main recomp_debug_main
/* Original main-thread startup, with a bounded desktop rendering mode. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "runtime.h"
#include "game.h"
#define require recomp_game_require
#include "recomp_audio.h"
#include "audio.h"
#include "renderer.h"
#include "graphics.h"
#include <time.h>
#include <errno.h>
static RecompRenderer *renderer;
static unsigned rendered_tasks;
static RecompTask last_graphics;
static unsigned mission_frames, first_mission_vi;
static uint32_t mission_difficulty;
static uint64_t mission_audio_underruns,max_drain_ns;
static unsigned next_input_frame, previous_input_frame;
static RecompPad replay_pad={.connected=true};
static bool has_next_input;
static bool cheat_menu;
static uint64_t cheat_unlock_checks;
static RecompFunction original_cheat_check;
static RecompFunctionEntry cheat_override;
#include "pager.h"
#include "generated.h"
#include "goldeneye_boot_config.h"
#include <dlfcn.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "boot_hooks.h"
static FILE *input_script;
static bool window_closed;

/* Optional call logging. Failure reports always retain the last 128 addresses. */
#define LOG_GOLDENEYE_CALLS (getenv("RECOMP_TRACE_CALLS") != NULL)
static GoldenEyePager pager;
static RecompAudio *audio;
static uint64_t now_ns;
static bool live_clock;
static uint64_t live_wall_origin,live_guest_origin;
static uint64_t monotonic_ns(void) {
    struct timespec t;
    require(clock_gettime(CLOCK_MONOTONIC,&t)==0,"GoldenEye wall clock unavailable",0);
    return (uint64_t)t.tv_sec*UINT64_C(1000000000)+(uint64_t)t.tv_nsec;
}
static uint32_t recent_calls[128];
static unsigned call_cursor;
static bool reached_graphics;
static RecompTask first_graphics;
static const char *report_path;
static uint32_t interrupt_mask=0x3fff01;
#include "services.h"
static uint64_t scheduler_batches;
static unsigned busy_frames;

static bool idle_only(Runtime *rt) {
    RecompThreadSnapshot threads[16];
    size_t count=recomp_thread_snapshots(rt,threads,16);
    if(count==16 || recomp_task_stats(rt).pending_records)return false;
    for(size_t i=0;i<count;i++) {
        const RecompThreadSnapshot *t=&threads[i];
        if(t->state!=RECOMP_READY && t->state!=RECOMP_RUNNING)continue;
        if((uint32_t)t->id!=BOOT_IDLE_THREAD_ID || t->priority!=0 || t->context.pc!=BOOT_IDLE_ENTRY)return false;
    }
    return true;
}
static void drain(Runtime *rt) {
    /* Retain the 4096-resume ceiling for useful work, but do not spend it on
     * the verified empty idle loop while every other guest thread is asleep. */
    for(unsigned batch=0;batch<256;batch++) {
        RecompRunResult run=recomp_scheduler_run(rt,16);scheduler_batches++;
        require(run!=RECOMP_RUN_COMPLETE,"GoldenEye guest threads ended unexpectedly",BOOT_MAIN_ENTRY);
        if(idle_only(rt))return;
    }
    busy_frames++;
}

static void unlocked_cheat_check(Runtime *rt,MipsContext *ctx) {
    uint32_t id=(uint32_t)ctx->gpr[4];
    if(id<64 && ((GE_CHEAT_UNLOCK_MASK>>id)&1)) {
        mips_set(ctx,2,1);cheat_unlock_checks++;
    } else original_cheat_check(rt,ctx);
}
static void install_cheat_menu(Runtime *rt) {
    for(size_t i=0;i<rt->function_count;i++) {
        const RecompFunctionEntry *entry=rt->function_index[i];
        if(entry->address!=GE_CHEAT_ENTRY)continue;
        original_cheat_check=entry->function;cheat_override=*entry;
        cheat_override.function=unlocked_cheat_check;rt->function_index[i]=&cheat_override;
        puts("All Solo missions and 23 Cheat Options unlocked for this session. Choose a file, then Solo to select a mission.");
        return;
    }
    recomp_fail("GoldenEye cheat query not registered",GE_CHEAT_ENTRY);
}
static void read_next_input(void) {
    unsigned buttons;int x,y;
    int read=fscanf(input_script,"%u %u %d %d",&next_input_frame,&buttons,&x,&y);
    if(read==EOF){require(!ferror(input_script),"GoldenEye controller script read failed",0);has_next_input=false;return;}
    require(read==4 && next_input_frame>previous_input_frame && next_input_frame<=36000
            && buttons<=65535 && x>=-128 && x<=127 && y>=-128 && y<=127,
            "GoldenEye controller script event invalid",next_input_frame);
    previous_input_frame=next_input_frame;has_next_input=true;
    /* A pending event is applied only on its requested VI. */
    replay_pad.buttons=(uint16_t)buttons;replay_pad.stick_x=(int8_t)x;replay_pad.stick_y=(int8_t)y;
}

static void prepare_call(Runtime *rt, uint32_t address) {
    recent_calls[call_cursor++ % 128]=address;
    if (LOG_GOLDENEYE_CALLS) fprintf(stderr,"GoldenEye call 0x%08x\n",address);
    goldeneye_pager_prepare_call(rt,address);
}

static uint64_t guest_now(void *unused) {
    (void)unused;
    if(live_clock)return live_guest_origin+monotonic_ns()-live_wall_origin;
    return now_ns;
}
static bool submit(Runtime *,void *,const RecompTask *);
static void initialize_services(Runtime *rt) {
    /* Until the original inflater runs, SDK globals overlap compressed input.
     * Install native mirrors only after that input has become initialized data. */
    RecompClockConfig clock={.ticks_per_second=BOOT_COUNTER_HZ,.now_ns=guest_now,.timer_sentinel=BOOT_TIMER_SENTINEL};
    require(recomp_clock_init(rt,&clock) && recomp_scheduler_init(rt,&ge_scheduler_config)
            && recomp_platform_init(rt,&ge_platform_config),"GoldenEye native services init failed",0);
    const char *save=getenv("RECOMP_GOLDENEYE_SAVE");
    RecompIOConfig io={.connected_mask=1,.eeprom_size=512,.eeprom_path=save && *save?save:NULL};
    require(recomp_io_init(rt,&io),"GoldenEye native IO init failed",0);
    if(cheat_menu)recomp_store(rt,GE_CHEAT_LEVELS_ADDRESS,1,4);
    RecompAudioConfig ac={.output_enabled=true,.microcode=RECOMP_AUDIO_ABI1_LINEAR,.prebuffer_ms=160,.task_profile=&goldeneye_audio_task_profile};
    ac.playback=!(getenv("RECOMP_AUDIO_MUTE") && !strcmp(getenv("RECOMP_AUDIO_MUTE"),"1"));
    ac.wav_path=getenv("RECOMP_GOLDENEYE_WAV");
    audio=recomp_audio_create(rt,&ac);
    require(audio!=NULL,"GoldenEye native audio init failed",0);
    RecompTaskBackend tasks={.submit=submit};
    require(recomp_tasks_init(rt,&tasks),"GoldenEye task service init failed",0);
}
static bool submit(Runtime *rt,void *unused,const RecompTask *task) {
    (void)unused; (void)rt;
    if(task->words[0]==2) {
        require(recomp_audio_submit(audio,task),"GoldenEye audio microcode needs native support",task->words[4]);
        return true;
    }
    require(task->words[0]==1,"GoldenEye unsupported startup task",task->guest_address);
    if(!reached_graphics)first_graphics=*task;
    reached_graphics=true;
    require(recomp_renderer_submit(renderer,task),"GoldenEye graphics microcode unsupported",task->words[4]);
    rendered_tasks++;
    last_graphics=*task;
    if(recomp_load(rt,GE_INSPECT_STAGE_ADDRESS,4)==GE_INSPECT_DAM_STAGE) {
        if(!mission_frames){
            first_mission_vi=(unsigned)recomp_vi_state(rt).retraces;
            mission_difficulty=(uint32_t)recomp_load(rt,GE_INSPECT_DIFFICULTY_ADDRESS,4);
            mission_audio_underruns=recomp_audio_stats(audio).queue_underruns;
        }
        mission_frames++;
    }
    return true;
}

static void quoted(FILE *file,const char *text) {
    fputc('"',file);
    for(const unsigned char *p=(const unsigned char *)text;*p;p++) {
        if(*p=='"'||*p=='\\')fputc('\\',file);
        if(*p<32)fprintf(file,"\\u%04x",*p);else fputc(*p,file);
    }
    fputc('"',file);
}
static void failure(const RecompFailureInfo *info,void *opaque) {
    Runtime *rt=opaque;char path[2048],ram_path[2048];
    if(snprintf(path,sizeof(path),"%s.crash.json",report_path)>=(int)sizeof(path)
       ||snprintf(ram_path,sizeof(ram_path),"%s.rdram.bin",report_path)>=(int)sizeof(ram_path))return;
    FILE *f=fopen(ram_path,"wb");if(f){(void)fwrite(rt->rdram,1,rt->rdram_size,f);fclose(f);}
    f=fopen(path,"w");if(!f)return;
    fputs("{\"format_version\":1,\"failure\":{\"message\":",f);quoted(f,info->message);
    fprintf(f,",\"address\":\"0x%08x\",\"requested_bytes\":%zu,\"ram_range\":%s},"
              "\"game\":{\"name\":\"GoldenEye startup\"},\"threads\":[",
              info->address,info->requested_bytes,info->ram_range?"true":"false");
    RecompThreadSnapshot threads[16];size_t count=recomp_thread_snapshots(rt,threads,16);
    if(count>16)count=16;
    for(size_t i=0;i<count;i++) {
        const RecompThreadSnapshot *t=&threads[i];
        fprintf(f,"%s{\"id\":%d,\"priority\":%d,\"current\":%s,\"state\":%u,"
                  "\"last_tracked_pc\":\"0x%08x\",\"gpr\":[",i?",":"",t->id,t->priority,
                  t->current?"true":"false",(unsigned)t->state,t->context.pc);
        for(unsigned j=0;j<32;j++)fprintf(f,"%s\"0x%016" PRIx64 "\"",j?",":"",t->context.gpr[j]);
        fputs("]}",f);
    }
    fputs("],\"native_stack\":[",f);
    for(size_t i=0;i<info->native_frame_count;i++) {
        Dl_info d={0};(void)dladdr(info->native_frames[i],&d);
        fprintf(f,"%s{\"address\":\"0x%" PRIxPTR "\",\"module_base\":\"0x%" PRIxPTR "\",\"module\":",
                i?",":"",(uintptr_t)info->native_frames[i],(uintptr_t)d.dli_fbase);
        quoted(f,d.dli_fname?d.dli_fname:"");fputs(",\"symbol\":",f);quoted(f,d.dli_sname?d.dli_sname:"");fputc('}',f);
    }
    fputs("],\"recent_calls\":[",f);
    unsigned retained=call_cursor<128?call_cursor:128;
    for(unsigned i=0;i<retained;i++)fprintf(f,"%s\"0x%08x\"",i?",":"",recent_calls[(call_cursor-retained+i)%128]);
    fputs("],\"task_words\":[",f);
    RecompTask task;
    if(recomp_task_snapshot(rt,&task))for(unsigned i=0;i<16;i++)fprintf(f,"%s%u",i?",":"",task.words[i]);
    fprintf(f,"],\"pager\":{\"loads\":%" PRIu64 ",\"evictions\":%" PRIu64 ",\"calls\":%" PRIu64 "},"
              "\"rdram\":{\"storage\":\"word-swapped\",\"path\":",pager.loads,pager.evictions,pager.calls);
    quoted(f,ram_path);fputs("}}\n",f);fclose(f);
    fprintf(stderr,"GoldenEye crash report: %s\n",path);
}

int main(int argc,char **argv) {
    if(argc!=5 && (argc!=6 || strcmp(argv[5],"--cheat-menu"))){fprintf(stderr,"usage: %s ROM DATA_CAPTURE STARTUP_REPORT FRAMES [--cheat-menu]\n",argv[0]);return 2;}
    cheat_menu=argc==6;
    char *end;unsigned long frames=strtoul(argv[4],&end,10);
    if(*end || frames>36000)return 2;
    data_output=argv[2];report_path=argv[3];
    FILE *f=fopen(argv[1],"rb");require(f!=NULL,"cannot open GoldenEye ROM",0);
    uint8_t *rom=malloc(BOOT_ROM_SIZE);require(rom!=NULL,"cannot allocate GoldenEye ROM",0);
    bool read=fread(rom,1,BOOT_ROM_SIZE,f)==BOOT_ROM_SIZE;int extra=fgetc(f),closed=fclose(f);
    require(read && extra==EOF && closed==0,"GoldenEye ROM read failed",0);
    Runtime rt;
    require(recomp_runtime_init(&rt,BOOT_RAM_SIZE,recomp_functions,recomp_function_count,
                               recomp_boot_sections,recomp_section_count),"GoldenEye runtime init failed",0);
    if(cheat_menu)install_cheat_menu(&rt);
    recomp_set_failure_handler(failure,&rt);
    require(recomp_rom_attach(&rt,rom,BOOT_ROM_SIZE,boot_sha1),"GoldenEye ROM identity mismatch",0);
    char renderer_data[2048];
    require(snprintf(renderer_data,sizeof(renderer_data),"%s.rt64",report_path)<(int)sizeof(renderer_data),"GoldenEye render path too long",0);
    renderer=recomp_renderer_create_named(&rt,renderer_data,"Native GoldenEye 007 recompilation");
    require(renderer!=NULL,"GoldenEye renderer initialization failed",0);
    recomp_renderer_set_graphics_hook(renderer,goldeneye_configure_graphics);
    const char *script=getenv("RECOMP_GOLDENEYE_INPUT");
    if(script && *script){input_script=fopen(script,"r");require(input_script!=NULL,"cannot open GoldenEye controller script",0);read_next_input();}
    recomp_copy_to_ram(&rt,BOOT_INITIAL_LOAD_ADDRESS,rom+BOOT_INITIAL_ROM_OFFSET,BOOT_INITIAL_LOAD_SIZE);
    for(uint32_t a=BOOT_BSS_START;a<BOOT_BSS_END;a+=4)recomp_store(&rt,a,0xa5a5a5a5,4);
    for(size_t i=0;i<sizeof(boot_parameters)/sizeof(boot_parameters[0]);i++)
        recomp_store(&rt,boot_parameters[i][0],boot_parameters[i][1],4);
    goldeneye_pager_install(&rt,&pager,&ge_pager_config);
    rt.prepare_call=prepare_call;
    rt.section_loaded[BOOT_GAME_SECTION]=true;
    ge_initialize_services=initialize_services;
    /* Original boot can run directly on the host stack. osStartThread publishes
     * mainproc for the native scheduler, without a fabricated guest boot thread. */
    MipsContext boot={.fcr31=0x01000800};boot.gpr[31]=mips_s32(0xf0000000);
    recomp_lookup(&rt,BOOT_ENTRY)(&rt,&boot);
    struct timespec deadline;clock_gettime(CLOCK_MONOTONIC,&deadline);
    unsigned presentations=0;
    RecompPad scripted_pad={.connected=true};
    for(unsigned step=0;!frames || step<frames;step++) {
        uint64_t drain_start=monotonic_ns();
        drain(&rt);
        uint64_t drain_ns=monotonic_ns()-drain_start;
        if(drain_ns>max_drain_ns)max_drain_ns=drain_ns;
        // Finish SDK/queue initialization before exposing elapsed host time.
        // Thereafter VI and audio deadlines advance during long guest loads.
        if(live_clock)now_ns=guest_now(NULL);
        else {
            now_ns+=16666667;
            if(reached_graphics){live_guest_origin=now_ns;live_wall_origin=monotonic_ns();live_clock=true;}
        }
        RecompPad pad;
        if(!recomp_renderer_input(renderer,&pad)){window_closed=true;break;}
        if(input_script) {
            if(has_next_input && next_input_frame==step+1){scripted_pad=replay_pad;read_next_input();}
            pad=scripted_pad;
        }
        require(recomp_input_set(&rt,0,&pad),"GoldenEye input update failed",step);
        require(recomp_renderer_present(renderer),"GoldenEye presentation failed",step);
        presentations++;
        if(frames && ((step+1)%60==0 || step+1==frames)) {
            char capture[2048];
            require(snprintf(capture,sizeof(capture),"%s.frame-%04u.ppm",report_path,step+1)<(int)sizeof(capture),"GoldenEye capture path too long",0);
            require(recomp_renderer_capture(renderer,capture),"GoldenEye window capture failed",step);
            RecompViState vi=recomp_vi_state(&rt);
            printf("GoldenEye VI %u; graphics tasks %u; VI black %s; framebuffer %08x; list %u bytes.\n",step+1,rendered_tasks,vi.black?"yes":"no",vi.framebuffer,last_graphics.words[13]);fflush(stdout);
        }
        deadline.tv_nsec+=16666667;
        if(deadline.tv_nsec>=1000000000){deadline.tv_nsec-=1000000000;deadline.tv_sec++;}
        while(clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&deadline,NULL)==EINTR){}
    }
    if(input_script){fclose(input_script);input_script=NULL;}
    char final_capture[2048];
    require(snprintf(final_capture,sizeof(final_capture),"%s.window.ppm",report_path)<(int)sizeof(final_capture),"GoldenEye window capture path too long",0);
    if(reached_graphics)require(recomp_renderer_capture(renderer,final_capture),"GoldenEye final window capture failed",0);
    require(reached_graphics || window_closed,"GoldenEye startup exceeded its graphics budget",BOOT_MAIN_ENTRY);
    char ram_dump[2048];
    require(snprintf(ram_dump,sizeof(ram_dump),"%s.rdram.bin",report_path)<(int)sizeof(ram_dump),"GoldenEye RAM capture path too long",0);
    f=fopen(ram_dump,"wb");require(f!=NULL,"cannot create GoldenEye RAM capture",0);
    require(fwrite(rt.rdram,1,rt.rdram_size,f)==rt.rdram_size && fclose(f)==0,"cannot write GoldenEye RAM capture",0);
    f=fopen(report_path,"w");require(f!=NULL,"cannot open GoldenEye startup report",0);
    fprintf(f,"{\"main_state\":\"%s\","
              "\"gameplay_validated\":false,"
              "\"registered_functions\":%zu,\"virtual_time_ns\":%" PRIu64 ",\"scheduler_batches\":%" PRIu64 ",\"busy_frames\":%u,"
              "\"page_loads\":%" PRIu64 ",\"page_evictions\":%" PRIu64 ",\"paged_calls\":%" PRIu64 ","
              "\"first_graphics_object\":%u,\"first_graphics_words\":[",
              "rendered_startup",
              recomp_function_count,now_ns,scheduler_batches,busy_frames,pager.loads,pager.evictions,pager.calls,first_graphics.guest_address);
    for(unsigned i=0;i<16;i++)fprintf(f,"%s%u",i?",":"",first_graphics.words[i]);
    fputs("]",f);
    RecompRendererStats rs=recomp_renderer_stats(renderer);
    RecompIOStats io=recomp_io_stats(&rt);RecompAudioStats as=recomp_audio_stats(audio);
    fprintf(f,",\"rendered\":true,\"window_closed\":%s,\"graphics_tasks\":%u,\"presentations\":%u,\"sp_callbacks\":%" PRIu64 ",\"dp_callbacks\":%" PRIu64
              ",\"cheat_menu\":%s,\"unlocked_cheat_count\":%u,\"cheat_unlock_checks\":%" PRIu64
              ",\"missions_unlocked\":%s"
              ",\"controller_reads\":%" PRIu64 ",\"eeprom_reads\":%" PRIu64 ",\"eeprom_writes\":%" PRIu64 ",\"save_failures\":%" PRIu64
              ",\"audio_tasks\":%" PRIu64 ",\"pcm_frames\":%" PRIu64 ",\"nonzero_samples\":%" PRIu64 ",\"playback_frames\":%" PRIu64
              ",\"audio_underruns\":%" PRIu64 ",\"audio_queue_resets\":%" PRIu64
              ",\"audio_underruns_before_mission\":%" PRIu64 ",\"audio_frequency\":%u,\"audio_prebuffer_frames\":%u,\"audio_min_queued_bytes\":%u,\"max_drain_ns\":%" PRIu64
              ",\"mission_frames\":%u,\"first_mission_vi\":%u,\"mission_difficulty\":%u,\"current_stage\":%u,\"rdp_triangles\":%" PRIu64
              ",\"renderer_submit_ns\":%" PRIu64 ",\"renderer_present_ns\":%" PRIu64,
        window_closed?"true":"false",rendered_tasks,presentations,rs.sp_callbacks,rs.dp_callbacks,
        cheat_menu?"true":"false",cheat_menu?GE_CHEAT_COUNT:0,cheat_unlock_checks,
        recomp_load(&rt,GE_CHEAT_LEVELS_ADDRESS,4)?"true":"false",
        io.reads_completed,io.eeprom_reads,io.eeprom_writes,io.persistence_failures,as.tasks,as.frames,as.nonzero_samples,as.playback_frames,as.queue_underruns,as.queue_resets,
        mission_audio_underruns,as.frequency,as.prebuffer_frames,as.min_queued_bytes,max_drain_ns,
        mission_frames,first_mission_vi,mission_difficulty,(unsigned)recomp_load(&rt,GE_INSPECT_STAGE_ADDRESS,4),rs.rdp_triangles,rs.submit_ns,rs.present_ns);
    fputs("}\n",f);require(fclose(f)==0,"cannot close GoldenEye startup report",0);
    recomp_set_failure_handler(NULL,NULL);
    recomp_audio_destroy(audio);
    recomp_renderer_destroy(renderer);
    recomp_runtime_destroy(&rt);free(rom);
    puts("GoldenEye original startup rendered in the native window.");
    return 0;
}
