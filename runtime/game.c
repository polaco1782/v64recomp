#define _GNU_SOURCE
#include "game.h"
#include "game_defaults.h"
#include "profiler.h"
#include <dlfcn.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static const char *crash_dir;
static uint8_t rom_sha1[20];
static void directory(const char *path) {
    char buffer[4096];
    recomp_game_require(strlen(path)<sizeof(buffer), "game directory path too long", 0);
    strcpy(buffer,path);
    for(char *p=buffer+1;;p++) {
        if(*p!='/' && *p)continue;
        char saved=*p;*p=0;
        recomp_game_require(!mkdir(buffer,0755) || errno==EEXIST, "cannot create game directory", 0);
        *p=saved;if(!saved)break;
    }
}
int recomp_game_options(int argc,char **argv,RecompGameOptions *o) {
    const char *texture_source=NULL;
    const char *profile_directory=NULL;
    unsigned profile_hz=99,profile_delay=0,profile_seconds=0;
    *o=(RecompGameOptions){.rom=RECOMP_DEFAULT_ROM,.data=RECOMP_DEFAULT_DATA,
        .save=RECOMP_DEFAULT_SAVE,.crash_dir=RECOMP_DEFAULT_CRASH};
    for(int i=1;i<argc;i++) {
        const char *arg=argv[i];
        if(!strcmp(arg,"--help")) {
            printf("usage: %s [--rom FILE] [--data DIR] [--save FILE] [--crash-dir DIR] [--frames N] [--mute] [--texture-scan [ZIP_OR_DIRECTORY]]\n"
                "  --profile DIR          CPU totals, hot functions and call stacks in a new directory\n"
                "  --profile-hz N         CPU samples per second per busy thread (default 99, max 1000)\n"
                "  --profile-delay N      Wait N seconds before profiling (default 0)\n"
                "  --profile-seconds N    Stop profiling after N seconds; 0 = until exit (default)\n",argv[0]);
            return 1;
        }
        if(!strcmp(arg,"--mute")){o->mute=true;continue;}
        if(!strcmp(arg,"--texture-scan")) {
            texture_source=RECOMP_DEFAULT_TEXTURE_SOURCE;
            if(i+1<argc && argv[i+1][0]!='-')texture_source=argv[++i];
            struct stat source;
            if(stat(texture_source,&source) || (!S_ISDIR(source.st_mode) && !S_ISREG(source.st_mode))) {
                fprintf(stderr,"texture scan source must be a ZIP file or directory: %s\n",texture_source);
                return -1;
            }
            continue;
        }
        if(i+1>=argc) { fprintf(stderr,"missing value for %s\n",arg);return -1; }
        const char *value=argv[++i];
        if(!*value){fprintf(stderr,"empty value for %s\n",arg);return -1;}
        if(!strcmp(arg,"--rom"))o->rom=value;
        else if(!strcmp(arg,"--data"))o->data=value;
        else if(!strcmp(arg,"--save"))o->save=value;
        else if(!strcmp(arg,"--crash-dir"))o->crash_dir=value;
        else if(!strcmp(arg,"--profile"))profile_directory=value;
        else if(!strcmp(arg,"--profile-hz") || !strcmp(arg,"--profile-delay") || !strcmp(arg,"--profile-seconds")) {
            char *end;errno=0;unsigned long n=strtoul(value,&end,10);
            if(errno || *end || *value<'0' || *value>'9' || n>86400 ||
               (!strcmp(arg,"--profile-hz") && (!n || n>1000))) {
                fprintf(stderr,"invalid value for %s: %s\n",arg,value);return -1;
            }
            if(!strcmp(arg,"--profile-hz"))profile_hz=(unsigned)n;
            else if(!strcmp(arg,"--profile-delay"))profile_delay=(unsigned)n;
            else profile_seconds=(unsigned)n;
        }
        else if(!strcmp(arg,"--frames")) {
            char *end;errno=0;unsigned long n=strtoul(value,&end,10);
            if(errno || *end || *value<'0' || *value>'9' || n>UINT32_MAX) {
                fprintf(stderr,"invalid frame count: %s\n",value);return -1;
            }
            o->frames=(uint32_t)n;
        } else { fprintf(stderr,"unknown option: %s\n",arg);return -1; }
    }
    directory(o->data);directory(o->crash_dir);
    char parent[4096];
    if(strlen(o->save)>=sizeof(parent)) { fprintf(stderr,"save path too long\n");return -1; }
    strcpy(parent,o->save);char *slash=strrchr(parent,'/');
    if(slash && slash!=parent){*slash=0;directory(parent);}
    crash_dir=o->crash_dir;
    if(!getenv("RECOMP_TEXTURE_REPLACEMENTS"))setenv("RECOMP_TEXTURE_REPLACEMENTS",RECOMP_DEFAULT_TEXTURES,1);
    if(texture_source) {
        setenv("RECOMP_TEXTURE_SCAN",texture_source,1);
        if(!getenv("RECOMP_TEXTURE_SCAN_RAW"))setenv("RECOMP_TEXTURE_SCAN_RAW",RECOMP_DEFAULT_TEXTURE_RAW,1);
        const char *replacements=getenv("RECOMP_TEXTURE_REPLACEMENTS");
        if(!replacements || !*replacements)setenv("RECOMP_TEXTURE_REPLACEMENTS",RECOMP_DEFAULT_TEXTURES,1);
    }
    if(profile_directory && !recomp_profile_start(profile_directory,profile_hz,profile_delay,profile_seconds))return -1;
    return 0;
}
void recomp_game_require(bool condition,const char *message,uint32_t address) {
    if(!condition)recomp_fail(message,address);
}
uint64_t recomp_game_monotonic(void) {
    struct timespec ts;
    recomp_game_require(clock_gettime(CLOCK_MONOTONIC,&ts)==0,"host clock failed",0);
    return (uint64_t)ts.tv_sec*UINT64_C(1000000000)+(uint64_t)ts.tv_nsec;
}
void recomp_game_sleep(uint64_t deadline) {
    struct timespec ts={(time_t)(deadline/1000000000),(long)(deadline%1000000000)};
    while(clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&ts,NULL)==EINTR){}
}
static void quoted(FILE *f,const char *s) {
    fputc('"',f);
    for(const unsigned char *p=(const unsigned char *)(s?s:"");*p;p++) {
        if(*p=='"' || *p=='\\')fputc('\\',f);
        if(*p<32)fprintf(f,"\\u%04x",*p);else fputc(*p,f);
    }
    fputc('"',f);
}
static void failure(const RecompFailureInfo *info,void *opaque) {
    Runtime *rt=opaque;
    char path[4096],ram_path[4096],executable[4096];
    /* Keep earlier failures, including failures from simultaneous sessions. */
    uint64_t stamp=recomp_game_monotonic();
    if(snprintf(path,sizeof(path),"%s/crash-%ld-%" PRIu64 ".json",crash_dir,(long)getpid(),stamp)>=(int)sizeof(path)
       || snprintf(ram_path,sizeof(ram_path),"%s.rdram.bin",path)>=(int)sizeof(ram_path))return;
    FILE *f=fopen(ram_path,"wb");bool saved=false;
    if(f){saved=fwrite(rt->rdram,1,rt->rdram_size,f)==rt->rdram_size; saved=fclose(f)==0 && saved;}
    f=fopen(path,"w");if(!f){fprintf(stderr,"cannot write crash report: %s\n",path);return;}
    ssize_t n=readlink("/proc/self/exe",executable,sizeof(executable)-1);executable[n>=0?n:0]=0;
    fputs("{\"format_version\":1,\"executable\":",f);quoted(f,executable);
    fputs(",\"game\":{\"name\":",f);quoted(f,RECOMP_GAME_ID);
    fputs("},\"rom_sha1\":\"",f);
    for(unsigned i=0;i<20;i++)fprintf(f,"%02x",rom_sha1[i]);
    fputs("\",\"failure\":{\"message\":",f);quoted(f,info->message);
    fprintf(f,",\"address\":\"0x%08x\",\"requested_bytes\":%zu,\"ram_range\":%s},\"threads\":[",
        info->address,info->requested_bytes,info->ram_range?"true":"false");
    RecompThreadSnapshot threads[64];size_t count=recomp_thread_snapshots(rt,threads,64);
    if(count>64)count=64;
    for(size_t i=0;i<count;i++) {
        RecompThreadSnapshot *t=&threads[i];
        fprintf(f,"%s{\"object\":\"0x%08x\",\"id\":%d,\"state\":%u,\"priority\":%d,\"current\":%s,\"last_tracked_pc\":\"0x%08x\","
            "\"hi\":\"0x%016" PRIx64 "\",\"lo\":\"0x%016" PRIx64 "\",\"fcr31\":\"0x%08x\",\"fpu_fr1\":%s,\"gpr\":[",
            i?",":"",t->object,t->id,(unsigned)t->state,t->priority,t->current?"true":"false",t->context.pc,
            t->context.hi,t->context.lo,t->context.fcr31,t->context.fpu_fr1?"true":"false");
        for(unsigned j=0;j<32;j++)fprintf(f,"%s\"0x%016" PRIx64 "\"",j?",":"",t->context.gpr[j]);
        fputs("],\"fpr\":[",f);
        for(unsigned j=0;j<32;j++)fprintf(f,"%s\"0x%08x\"",j?",":"",t->context.fpr[j]);
        fputs("],\"fpr_high\":[",f);
        for(unsigned j=0;j<32;j++)fprintf(f,"%s\"0x%08x\"",j?",":"",t->context.fpr_high[j]);
        fputs("]}",f);
    }
    fputs("],\"loaded_sections\":[",f);
    for(size_t i=0;i<rt->section_count;i++)fprintf(f,"%s%s",i?",":"",rt->section_loaded[i]?"true":"false");
    fputs("],\"recent_calls\":[",f);
#ifdef RECOMP_DEBUG
    unsigned first=(rt->recent_call_next+128-rt->recent_call_count)%128;
    for(unsigned i=0;i<rt->recent_call_count;i++)
        fprintf(f,"%s\"0x%08x\"",i?",":"",rt->recent_calls[(first+i)%128]);
#endif
    fputs("],\"native_stack\":[",f);
    for(size_t i=0;i<info->native_frame_count;i++) {
        Dl_info d={0};(void)dladdr(info->native_frames[i],&d);
        fprintf(f,"%s{\"address\":\"0x%" PRIxPTR "\",\"module_base\":\"0x%" PRIxPTR "\",\"module\":",
            i?",":"",(uintptr_t)info->native_frames[i],(uintptr_t)d.dli_fbase);
        quoted(f,d.dli_fname);fputs(",\"symbol\":",f);quoted(f,d.dli_sname);fputc('}',f);
    }
    fputs("],\"task_words\":[",f);RecompTask task;
    if(recomp_task_snapshot(rt,&task))for(unsigned i=0;i<16;i++)fprintf(f,"%s%u",i?",":"",task.words[i]);
    fprintf(f,"],\"rdram\":{\"storage\":\"word-swapped\",\"size\":%zu,\"saved\":%s,\"path\":",rt->rdram_size,saved?"true":"false");
    quoted(f,ram_path);fputs("}}\n",f);
    bool okay=!ferror(f);okay=fclose(f)==0 && okay;
    fprintf(stderr,"Crash report %s: %s\n",okay?"written":"incomplete",path);
}
uint8_t *recomp_game_rom(Runtime *rt,const RecompGameOptions *o,size_t bytes,const uint8_t sha1[20]) {
    /* Positional diagnostic entry points may bypass recomp_game_options(). */
    if(o->crash_dir)crash_dir=o->crash_dir;
    if(!crash_dir)crash_dir=RECOMP_DEFAULT_CRASH;
    directory(crash_dir);
    memcpy(rom_sha1,sha1,sizeof(rom_sha1));
    recomp_set_failure_handler(failure,rt);
    FILE *f=fopen(o->rom,"rb");recomp_game_require(f!=NULL,"cannot open game ROM",0);
    uint8_t *rom=malloc(bytes);recomp_game_require(rom!=NULL,"cannot allocate game ROM",0);
    bool read=fread(rom,1,bytes,f)==bytes;int extra=fgetc(f),closed=fclose(f);
    recomp_game_require(read && extra==EOF && closed==0,"ROM read/size mismatch",0);
    recomp_game_require(recomp_rom_attach(rt,rom,bytes,sha1),"ROM identity mismatch",0);
    return rom;
}
