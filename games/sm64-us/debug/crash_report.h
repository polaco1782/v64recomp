/* Failure-path snapshots only. No guest instructions are interpreted here. */
#include <dlfcn.h>
#include <unistd.h>

static void crash_string(FILE *f,const char *value) {
    if(!value) { fputs("null",f);return; }
    fputc('"',f);
    for(const unsigned char *p=(const unsigned char *)value;*p;p++) {
        if(*p=='"' || *p=='\\') { fputc('\\',f);fputc(*p,f); }
        else if(*p<32)fprintf(f,"\\u%04x",*p);
        else fputc(*p,f);
    }
    fputc('"',f);
}
static uint32_t crash_word(const Runtime *rt,uint32_t address,unsigned bytes) {
    uint32_t p=address;
    if(p>=0x80000000 && p<0xc0000000)p&=0x1fffffff;
    if(!rt->rdram || p>=rt->rdram_size || bytes>rt->rdram_size-p)return 0;
    uint32_t value=0;
    for(unsigned n=0;n<bytes;n++)value=(value<<8)|rt->rdram[(p+n)^3];
    return value;
}
static void report_crash(const RecompFailureInfo *failure,void *opaque) {
    Runtime *rt=opaque;struct timespec time={0};(void)clock_gettime(CLOCK_REALTIME,&time);
    char prefix[4096],json_path[4096],ram_path[4096],executable[4096];
    int n=snprintf(prefix,sizeof(prefix),"%s/crash-%jd-%09ld",output,(intmax_t)time.tv_sec,time.tv_nsec);
    if(n<0 || (size_t)n>=sizeof(prefix)-11)return;
    memcpy(json_path,prefix,(size_t)n);memcpy(json_path+n,".json",6);
    memcpy(ram_path,prefix,(size_t)n);memcpy(ram_path+n,"-rdram.bin",11);
    ssize_t length=readlink("/proc/self/exe",executable,sizeof(executable)-1);
    executable[length>=0?length:0]='\0';
    bool ram_saved=false;
    FILE *ram=fopen(ram_path,"wb");
    if(ram) { ram_saved=fwrite(rt->rdram,1,rt->rdram_size,ram)==rt->rdram_size;ram_saved=fclose(ram)==0 && ram_saved; }
    FILE *f=fopen(json_path,"w");
    if(!f) { fprintf(stderr,"Cannot write crash report: %s\n",json_path);return; }
    fprintf(f,"{\n\"format_version\":1,\n\"failure\":{\"message\":");crash_string(f,failure->message);
    fprintf(f,",\"address\":\"0x%08x\",\"ram_range\":%s,\"requested_bytes\":%zu},\n\"executable\":",
        failure->address,failure->ram_range?"true":"false",failure->requested_bytes);
    crash_string(f,executable);
    fputs(",\n\"rom_sha1\":\"",f);for(unsigned i=0;i<20;i++)fprintf(f,"%02x",game_sha1[i]);
    fputs("\",\n\"game\":{\"mode\":",f);crash_string(f,mode);
    uint32_t mario=crash_word(rt,GAME_GMARIOSTATE,4),area=crash_word(rt,GAME_GCURRENTAREA,4);
    fprintf(f,",\"level\":%u,\"area\":%u,\"action\":\"0x%08x\",\"world_frames\":%u,\"play_frames\":%u,\"graphics_tasks\":%u,\"audio_tasks\":%u,\"buttons\":%u,\"stick_x\":%d,\"stick_y\":%d},\n",
        crash_word(rt,GAME_GCURRLEVELNUM,2),area?crash_word(rt,area,1):0,mario?crash_word(rt,mario+12,4):0,
        world_frames,play_frames,graphics,audio,input.buttons,input.stick_x,input.stick_y);
    fprintf(f,"\"rdram\":{\"bytes\":%zu,\"storage\":\"word-swapped\",\"path\":",rt->rdram_size);
    crash_string(f,ram_saved?ram_path:NULL);fputs("},\n\"loaded_sections\":[",f);
    for(size_t i=0;i<rt->section_count;i++)fprintf(f,"%s%s",i?",":"",rt->section_loaded[i]?"true":"false");
    fputs("],\n\"threads\":[",f);
    RecompThreadSnapshot threads[16];size_t count=recomp_thread_snapshots(rt,threads,16);
    const char *states[]={"stopped","ready","running","wait_send","wait_recv","dead"};
    for(size_t i=0;i<count;i++) {
        RecompThreadSnapshot *t=&threads[i];MipsContext *ctx=&t->context;
        fprintf(f,"%s{\"object\":\"0x%08x\",\"id\":%d,\"priority\":%d,\"current\":%s,\"state\":\"%s\",\"last_tracked_pc\":\"0x%08x\",\"hi\":\"0x%016" PRIx64 "\",\"lo\":\"0x%016" PRIx64 "\",\"fcr31\":\"0x%08x\",\"gpr\":[",
            i?",":"",t->object,t->id,t->priority,t->current?"true":"false",states[t->state],ctx->pc,ctx->hi,ctx->lo,ctx->fcr31);
        for(unsigned r=0;r<32;r++)fprintf(f,"%s\"0x%016" PRIx64 "\"",r?",":"",ctx->gpr[r]);
        fputs("],\"fpr\":[",f);for(unsigned r=0;r<32;r++)fprintf(f,"%s\"0x%08x\"",r?",":"",ctx->fpr[r]);
        fputs("]}",f);
    }
    fputs("],\n\"task\":",f);RecompTask task;
    if(recomp_task_snapshot(rt,&task)) {
        fprintf(f,"{\"object\":\"0x%08x\",\"token\":%" PRIu64 ",\"descriptor\":[",task.guest_address,task.token);
        for(unsigned i=0;i<16;i++)fprintf(f,"%s\"0x%08x\"",i?",":"",task.words[i]);
        fputs("]}",f);
    } else fputs("null",f);
    fputs(",\n\"audio\":",f);
#ifdef RECOMP_WITH_AUDIO
    if(native_audio) {
        RecompAudioSnapshot a=recomp_audio_snapshot(native_audio);
        fprintf(f,"{\"list\":\"0x%08x\",\"bytes\":%u,\"command_count\":%" PRIu64 ",\"history\":[",a.list,a.bytes,a.command_index);
        for(size_t i=0;i<a.history_count;i++)fprintf(f,"%s{\"address\":\"0x%08x\",\"w1\":\"0x%08x\",\"w2\":\"0x%08x\"}",
            i?",":"",a.history[i].address,a.history[i].w1,a.history[i].w2);
        fputs("]}",f);
    } else
#endif
    fputs("null",f);
    fputs(",\n\"native_stack\":[",f);
    for(size_t i=0;i<failure->native_frame_count;i++) {
        Dl_info symbol={0};(void)dladdr(failure->native_frames[i],&symbol);
        fprintf(f,"%s{\"address\":\"%p\",\"module_base\":\"%p\",\"module\":",i?",":"",failure->native_frames[i],symbol.dli_fbase);
        crash_string(f,symbol.dli_fname);fputs(",\"symbol\":",f);crash_string(f,symbol.dli_sname);fputc('}',f);
    }
    fputs("]\n}\n",f);bool okay=!ferror(f);okay=fclose(f)==0 && okay;
    if(trace)(void)fflush(trace);
    if(okay)fprintf(stderr,"Crash report: %s\nInspect: .venv/bin/python tools/inspect_crash.py '%s'\n",json_path,json_path);
}
