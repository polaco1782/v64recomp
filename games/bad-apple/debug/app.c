#include "demo.h"
#include "game_defaults.h"
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
static void diagnostics(Runtime *rt, RecompRenderer *renderer, RecompAudio *audio, uint32_t frames) {
    recomp_game_require(recomp_renderer_capture(renderer, RECOMP_DEBUG_OUTPUT "/final.ppm"),
                        "Bad Apple capture failed", 0);
    FILE *gray = fopen(RECOMP_DEBUG_OUTPUT "/final.i8", "wb");
    recomp_game_require(gray != NULL, "Bad Apple grayscale dump failed", 0);
    for (uint32_t i = 0; i < 320 * 240; i++)
        recomp_game_require(fputc((int)recomp_load(rt, 0x80380000 + i, 1), gray) != EOF,
                            "Bad Apple grayscale dump write failed", 0);
    recomp_game_require(fclose(gray) == 0, "Bad Apple grayscale dump close failed", 0);
    RecompAudioStats stats = recomp_audio_stats(audio);
    fprintf(stderr, "Bad Apple: frames=%u audio_buffers=%llu nonzero_samples=%llu peak=%u underruns=%llu resets=%llu\n",
            frames, (unsigned long long)stats.ai_buffers, (unsigned long long)stats.nonzero_samples,
            stats.peak, (unsigned long long)stats.queue_underruns, (unsigned long long)stats.queue_resets);
}
int recomp_debug_main(int argc, char **argv) {
    if (argc != 5)
        return 2;
    /* Shared debug launcher supplies ROM, report path, frames and data path. */
    char *args[] = {argv[0], "--rom", argv[1], "--frames", argv[3], "--data", argv[4], "--mute", NULL};
    const char *wav = getenv("RECOMP_BAD_WAV");
    return bad_apple_main(getenv("RECOMP_AUDIO_MUTE") ? 8 : 7, args,
                          diagnostics, wav ? wav : RECOMP_DEBUG_OUTPUT "/audio.wav");
}
