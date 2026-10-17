#ifndef CONKER_H
#define CONKER_H
#include "game.h"
#include <setjmp.h>
#include "renderer.h"
#include "recomp_audio.h"

typedef struct Conker {
    Runtime runtime;
    RecompGameOptions options;
    RecompRenderer *renderer;
    RecompAudio *audio;
    uint8_t *rom;
    uint64_t now_ns, wall_origin, start_ns, next_ns;
    unsigned graphics_tasks, audio_tasks, page_loads, page_evictions;
    uint32_t boot_status;
    jmp_buf boot_return;
    bool booting;
} Conker;
void conker_start(Conker *, const RecompGameOptions *);
void conker_step(Conker *, const RecompPad *);
void conker_stop(Conker *);
#endif
