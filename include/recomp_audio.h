#ifndef RECOMP_AUDIO_H
#define RECOMP_AUDIO_H
#include "runtime.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct RecompAudio RecompAudio;
extern const struct RecompRspProgram game_audio_rsp_program;
typedef struct RecompAudioTaskProfile {
    uint32_t descriptor[6];        /* Expected boot/code/data addresses and task sizes. */
    uint32_t fingerprint_bytes[3]; /* Initialized bytes, excluding DMEM BSS. */
    const char *sha256[3];
    const struct RecompRspProgram *rsp;
    bool musyx_subframes; /* Task size counts 0xa10-byte subframes instead of command bytes. */
} RecompAudioTaskProfile;
typedef struct RecompAudioConfig {
    const char *wav_path;                       /* Optional little-endian stereo PCM capture. */
    bool playback;                              /* SDL device is opened lazily on the first active AI buffer. */
    bool output_enabled;                        /* Disable during accelerated startup; synthesis continues. */
    const RecompAudioTaskProfile *task_profile; /* Game-owned static recognition data; required for submit. */
    unsigned prebuffer_ms;                      /* Initial reserve; zero uses 160 ms, maximum 200 ms. */
    const char *trace_path;                     /* Optional CSV of AI admission and playback queue timing. */
} RecompAudioConfig;
typedef struct RecompAudioStats {
    uint64_t tasks, commands, ai_buffers, frames, nonzero_samples, clipped_samples, synth_ns;
    uint64_t playback_frames, queue_resets, queue_underruns;
    uint32_t frequency, peak, queued_bytes, min_queued_bytes, max_queued_bytes, prebuffer_frames;
    bool device_open, playback_running;
    char driver[32];
} RecompAudioStats;
RecompAudio *recomp_audio_create(Runtime *, const RecompAudioConfig *);
/* Owner-thread API with guest execution parked. Submit synthesizes directly
 * into live RAM before queuing SP completion; polling delivers the guest event.
 * SDL playback remains asynchronous. Destroy before Runtime and SDL_Quit. */
void recomp_audio_destroy(RecompAudio *);
void recomp_audio_set_output_enabled(RecompAudio *, bool);
bool recomp_audio_submit(RecompAudio *, const RecompTask *);
RecompAudioStats recomp_audio_stats(RecompAudio *);
#ifdef __cplusplus
}
#endif
#endif
