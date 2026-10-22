#ifndef PERFECTDARK_H
#define PERFECTDARK_H
#include "game.h"
#include "renderer.h"
#include "recomp_audio.h"

typedef struct PerfectDark {
    Runtime runtime; /* First member so guest service callbacks can find their owner. */
    RecompGameOptions options;
    RecompRenderer *renderer;
    RecompAudio *audio;
    uint8_t *rom;
    uint64_t now_ns, wall_origin, start_ns, next_ns;
    unsigned graphics_tasks, audio_tasks;
} PerfectDark;

/* Both frontends use the original bootstrap, native services and frame timing. */
void perfectdark_start(PerfectDark *, const RecompGameOptions *);
void perfectdark_step(PerfectDark *, const RecompPad *);
void perfectdark_stop(PerfectDark *);
#endif
