#include "recomp_audio.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"audio worker check: %s:%d: %s\n",__FILE__,__LINE__,#x);abort(); } } while(0)
static RecompAudio *audio;
static bool submit(Runtime *rt,void *opaque,const RecompTask *task) {
    (void)rt;(void)opaque;return recomp_audio_submit(audio,task);
}
int main(void) {
    Runtime rt;bool loaded=true;
    CHECK(recomp_runtime_init(&rt,65536,NULL,0,&loaded,1));
    RecompSchedulerConfig scheduler={.thread_tail=0x100};CHECK(recomp_scheduler_init(&rt,&scheduler));
    uint8_t rom[64]={0};rt.rom=rom;rt.rom_size=sizeof(rom);
    RecompClockConfig clock={.ticks_per_second=93750000};CHECK(recomp_clock_init(&rt,&clock));
    RecompPlatformConfig platform={.vi_period_ticks=1562500};CHECK(recomp_platform_init(&rt,&platform));
    uint8_t zeros[16]={0},digest[32];unsigned size;char hex[65];
    CHECK(EVP_Digest(zeros,16,digest,&size,EVP_sha256(),NULL) && size==32);
    for(unsigned i=0;i<32;i++)snprintf(hex+2*i,3,"%02x",digest[i]);
    RecompAudioTaskProfile profile={.descriptor={0x1000,16,0x1100,16,0x1200,16},
        .fingerprint_bytes={16,16,16},.sha256={hex,hex,hex}};
    RecompAudioConfig config={.microcode=RECOMP_AUDIO_ABI1,.task_profile=&profile};
    audio=recomp_audio_create(&rt,&config);CHECK(audio);
    const uint32_t commands[]={0x08000000,16,0x02000000,16,0x06000000,0x2000};
    for(unsigned i=0;i<6;i++)recomp_store(&rt,0x1800+4*i,commands[i],4);
    /* The independent DSP fixture API remains usable before task init. */
    memset(rt.rdram+0x2000,0x7f,16);
    recomp_audio_execute_abi1(audio,0x1800,24);
    for(unsigned i=0;i<16;i++)CHECK(rt.rdram[0x2000+i]==0);
    RecompTaskBackend backend={.submit=submit};CHECK(recomp_tasks_init(&rt,&backend));
    RecompTask task={.words={2,0,0x1000,16,0x1100,16,0x1200,16,0,0,0,0,0x1800,24}};
    for(unsigned i=0;i<16;i++)recomp_store(&rt,0xc00+4*i,task.words[i],4);
    for(unsigned repeat=0;repeat<100;repeat++) {
        memset(rt.rdram+0x2000,0x7f,16);
        MipsContext ctx={0};ctx.gpr[4]=0xc00;
        hook_os_sp_task_load(&rt,&ctx);hook_os_sp_task_start_go(&rt,&ctx);
        /* Live guest writes outside the DSP output survive snapshot commit. */
        recomp_store(&rt,0x3000,repeat+1,4);
        CHECK(recomp_task_stats(&rt).pending_records==1);
        recomp_task_services_flush(&rt);rt.poll_tasks(&rt);
        for(unsigned i=0;i<16;i++)CHECK(rt.rdram[0x2000+i]==0);
        CHECK(recomp_load(&rt,0x3000,4)==repeat+1);
        CHECK(recomp_task_stats(&rt).pending_records==0);
    }
    CHECK(recomp_audio_stats(audio).tasks==101);
    CHECK(recomp_task_stats(&rt).sp_events==100 && recomp_task_stats(&rt).rejected_completions==0);
    recomp_audio_destroy(audio);recomp_runtime_destroy(&rt);
    puts("asynchronous ABI1 output merge and SP receipts passed");return 0;
}
