#ifndef RECOMP_GAME_H
#define RECOMP_GAME_H
#include "runtime.h"

typedef struct RecompGameOptions {
    const char *rom, *data, *save, *crash_dir;
    uint32_t frames;
    bool mute;
} RecompGameOptions;

/* Defaults are configured by CMake; a built executable needs no Python. */
int recomp_game_options(int argc, char **argv, RecompGameOptions *options);
uint8_t *recomp_game_rom(Runtime *rt, const RecompGameOptions *options,
                        size_t bytes, const uint8_t sha1[20]);
uint64_t recomp_game_monotonic(void);
void recomp_game_sleep(uint64_t deadline);
void recomp_game_require(bool condition, const char *message, uint32_t address);
#endif
