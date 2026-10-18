#pragma once
#include "game.h"
#include "renderer.h"
#include "recomp_audio.h"

typedef struct Hexen {
    Runtime runtime;
    uint8_t *rom;
    RecompRenderer *renderer;
    RecompAudio *audio;
    uint64_t wall_origin;
    bool bss_verified, program_verified, helper_verified;
} Hexen;
void hexen_init(Hexen *, const RecompGameOptions *, const char *wav);
void hexen_step(Hexen *, const RecompPad *);
void hexen_pace(Hexen *, uint64_t deadline);
void hexen_destroy(Hexen *);
