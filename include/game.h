#ifndef RECOMP_GAME_H
#define RECOMP_GAME_H
#include "runtime.h"
#include "renderer.h"
#include "recomp_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct RecompGameOptions {
    const char *rom, *data, *save, *crash_dir;
    uint32_t frames;
    bool mute;
#ifdef RECOMP_DEBUG
    const char *input, *wav, *audio_trace;
    uint32_t capture_every;
    bool trace_calls;
#endif
} RecompGameOptions;

/* Defaults are configured by CMake; a built executable needs no Python. */
int recomp_game_options(int argc, char **argv, RecompGameOptions *options);
#ifdef RECOMP_DEBUG
/* Input adapters disable live mouse overrides during controller replay. */
bool recomp_game_replaying(void);
#endif
/* Common game loop operations. Debug adds controller replay, frame capture and
 * audio recording; Release forwards directly to the runtime/backends. Replay
 * overrides controller zero after game-specific host input mapping. */
bool recomp_game_input(Runtime *rt, unsigned controller, const RecompPad *pad);
bool recomp_game_present(RecompRenderer *renderer);
RecompAudio *recomp_game_audio_create(Runtime *rt, const RecompAudioConfig *config);
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
