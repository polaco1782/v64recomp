#ifndef RECOMP_AUDIO_H
#define RECOMP_AUDIO_H
#include "runtime.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct RecompAudio RecompAudio;
typedef enum RecompAudioMicrocode {
    RECOMP_AUDIO_ABI1 = 0,
    RECOMP_AUDIO_ABI1_LINEAR = 1,
    RECOMP_AUDIO_NEAD_OOT = 2,
    RECOMP_AUDIO_NAUDIO_MP3 = 3,
    RECOMP_AUDIO_NAUDIO_CBFD = 4,
    RECOMP_AUDIO_NAUDIO = 5,
    RECOMP_AUDIO_MUSYX_V1 = 6,
    RECOMP_AUDIO_NEAD_1080 = 7,
} RecompAudioMicrocode;
typedef struct RecompAudioTaskProfile {
    uint32_t descriptor[6];        /* Expected boot/code/data addresses and task sizes. */
    uint32_t fingerprint_bytes[3]; /* Initialized bytes, excluding DMEM BSS. */
    const char *sha256[3];
} RecompAudioTaskProfile;
typedef struct RecompAudioConfig {
    const char *wav_path; /* Optional little-endian stereo PCM capture. */
    bool playback;        /* SDL device is opened lazily on the first active AI buffer. */
    bool output_enabled;  /* Disable during accelerated startup; synthesis continues. */
    RecompAudioMicrocode microcode;
    const RecompAudioTaskProfile *task_profile; /* Game-owned static recognition data; required for submit. */
    unsigned prebuffer_ms;                      /* Zero uses 80 ms; a game may request up to 200 ms. */
    const char *trace_path;                     /* Optional CSV of AI admission and playback queue timing. */
} RecompAudioConfig;
typedef struct RecompAudioStats {
    uint64_t tasks, commands, ai_buffers, frames, nonzero_samples, clipped_samples, synth_ns;
    uint64_t opcodes[32], playback_frames, queue_resets, queue_underruns;
    uint32_t frequency, peak, queued_bytes, min_queued_bytes, max_queued_bytes, prebuffer_frames;
    bool device_open, playback_running;
    char driver[32];
} RecompAudioStats;
enum { RECOMP_AUDIO_HISTORY = 16 };
typedef struct RecompAudioCommand {
    uint32_t address, w1, w2;
} RecompAudioCommand;
typedef struct RecompAudioSnapshot {
    uint32_t list, bytes;
    uint64_t command_index;
    size_t history_count;
    RecompAudioCommand history[RECOMP_AUDIO_HISTORY];
} RecompAudioSnapshot;
RecompAudio *recomp_audio_create(Runtime *, const RecompAudioConfig *);
/* Owner-thread API with guest execution parked. Submit synthesizes directly
 * into live RAM before queuing SP completion; polling delivers the guest event.
 * SDL playback remains asynchronous. Destroy before Runtime and SDL_Quit. */
void recomp_audio_destroy(RecompAudio *);
void recomp_audio_set_output_enabled(RecompAudio *, bool);
bool recomp_audio_submit(RecompAudio *, const RecompTask *);
/* Explicit ABI1 DSP entry point, without microcode recognition or task receipts.
 * Useful for independent signal fixtures. Normal game tasks use submit(). */
void recomp_audio_execute_abi1(RecompAudio *, uint32_t commands, uint32_t bytes);
RecompAudioStats recomp_audio_stats(RecompAudio *);
RecompAudioSnapshot recomp_audio_snapshot(const RecompAudio *);
#ifdef __cplusplus
}
#endif
#endif
