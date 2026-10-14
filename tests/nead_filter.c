#include "recomp_audio.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "NEAD filter check failed: %s:%d: %s\n", __FILE__, __LINE__, #x); \
    abort(); \
} } while (0)

enum { LIST=0x1000, SOURCE=0x2000, STATE=0x2100, INPUT=0x2200 };
static void command(Runtime *rt, unsigned at, unsigned w1, unsigned w2) {
    recomp_store(rt, at, w1, 4);
    recomp_store(rt, at+4, w2, 4);
}

int main(void) {
    Runtime rt;
    bool loaded=true;
    CHECK(recomp_runtime_init(&rt, 65536, NULL, 0, &loaded, 1));
    RecompSchedulerConfig sc={.thread_tail=0xb00};
    CHECK(recomp_scheduler_init(&rt, &sc));
    RecompClockConfig cc={.ticks_per_second=46875000};
    CHECK(recomp_clock_init(&rt, &cc));
    static const uint8_t rom[64]={0};
    rt.rom=rom; rt.rom_size=sizeof(rom);
    RecompPlatformConfig pc={.vi_period_ticks=781250};
    CHECK(recomp_platform_init(&rt, &pc));
    RecompAudioConfig ac={.microcode=RECOMP_AUDIO_NEAD_OOT};
    RecompAudio *audio=recomp_audio_create(&rt, &ac);
    CHECK(audio);

    for (unsigned i=0; i<8; i++) {
        recomp_store(&rt, SOURCE+i*2, 0x2000, 2);
        recomp_store(&rt, INPUT+i*2, i+1, 2);
    }
    for (unsigned i=0; i<32; i++) recomp_store(&rt, STATE+i, 0x77, 1);
    // Load 16 bytes of samples, set up FILTER, then execute with INIT.
    command(&rt, LIST, 0x14010080, INPUT);
    command(&rt, LIST+8, 0x07020010, SOURCE);
    command(&rt, LIST+16, 0x07010080, STATE);
    recomp_audio_execute_abi1(audio, LIST, 24);
    for (unsigned i=0; i<8; i++) {
        CHECK(recomp_load(&rt, SOURCE+i*2, 2)==0x2000);
        CHECK(recomp_load(&rt, STATE+16+i*2, 2)==0x1000);
        CHECK(recomp_load(&rt, STATE+i*2, 2)==i+1);
        // The CPU may prepare the following task's coefficients concurrently.
        recomp_store(&rt, SOURCE+i*2, 0x4000, 2);
    }

    // Without setup, the cached RSP coefficients remain active.
    command(&rt, LIST, 0x07000080, STATE);
    recomp_audio_execute_abi1(audio, LIST, 8);
    for (unsigned i=0; i<8; i++) {
        CHECK(recomp_load(&rt, SOURCE+i*2, 2)==0x4000);
        CHECK(recomp_load(&rt, STATE+16+i*2, 2)==0x1000);
    }

    // Another setup explicitly reloads the changed source table.
    command(&rt, LIST, 0x07020010, SOURCE);
    command(&rt, LIST+8, 0x07000080, STATE);
    recomp_audio_execute_abi1(audio, LIST, 16);
    for (unsigned i=0; i<8; i++) {
        CHECK(recomp_load(&rt, SOURCE+i*2, 2)==0x4000);
        CHECK(recomp_load(&rt, STATE+16+i*2, 2)==0x2800);
    }
    recomp_audio_destroy(audio);
    recomp_runtime_destroy(&rt);
    puts("PASS: NEAD filter source ownership, INIT history and coefficient caching");
    return 0;
}
