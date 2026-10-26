#ifndef RECOMP_GAME_H
#define RECOMP_GAME_H
#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RecompGameOptions {
    const char *rom, *data, *save, *crash_dir;
    uint32_t frames;
    bool mute;
} RecompGameOptions;

/* Defaults are configured by CMake; a built executable needs no Python. */
int recomp_game_options(int argc, char **argv, RecompGameOptions *options);
/* Load/verify external or embedded ROM content; the caller owns the allocation.
 * Embedded builds default to the sanitized image; --rom selects an original ROM.
 * Install the common crash reporter for this runtime.
 * Games report fatal errors with recomp_fail() or recomp_game_require(), and
 * recoverable skipped operations with recomp_warn(). */
uint8_t *recomp_game_rom(Runtime *rt, const RecompGameOptions *options, size_t bytes, const uint8_t sha1[20]);
uint64_t recomp_game_monotonic(void);
void recomp_game_sleep(uint64_t deadline);
/* Run guest work and service VI/AI/task completion while pacing to a host
 * presentation deadline, waking at the earlier of hardware deadlines and 1 ms.
 * Configure the game's verified idle thread and enter its wall-clock phase
 * first. Keep synthetic-clock bootstrap and frame-exact diagnostics separate. */
void recomp_game_pace(Runtime *rt, uint64_t deadline);
void recomp_game_require(bool condition, const char *message, uint32_t address);
#ifdef __cplusplus
}
#endif
#endif
