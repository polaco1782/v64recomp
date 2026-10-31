#include "player.h"
#include "game.h"
#include <stdio.h>
#include <stdlib.h>

static FILE *wav;
static uint32_t wav_bytes, wav_rate;
static bool capture_stopped;
static void le32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8*i));
}
static void capture(const int16_t *pcm, int samples, int rate) {
    if (!wav || capture_stopped) return;
    if (wav_rate && wav_rate != (uint32_t)rate) {
        fputs("MP3: WAV capture stopped at sample-rate change; playback continues\n", stderr);
        capture_stopped = true;
        return;
    }
    wav_rate = (uint32_t)rate;
    recomp_game_require(wav_bytes <= UINT32_MAX-44-(uint32_t)samples*4, "WAV capture exceeds 4 GB", 0);
    uint8_t bytes[4608];
    for (int i = 0; i < samples*2; i++) {
        bytes[2*i] = (uint8_t)pcm[i]; bytes[2*i+1] = (uint8_t)((uint16_t)pcm[i] >> 8);
    }
    recomp_game_require(fwrite(bytes, 4, samples, wav) == (size_t)samples, "WAV capture write failed", 0);
    wav_bytes += (uint32_t)samples*4;
}
int recomp_debug_main(int argc, char **argv) {
    if (argc != 5) return 2;
    const char *path = getenv("RECOMP_MEDIA_WAV");
    if (path && *path) {
        wav = fopen(path, "wb");
        recomp_game_require(wav != NULL, "WAV capture open failed", 0);
        uint8_t empty[44] = {0};
        recomp_game_require(fwrite(empty, 1, sizeof(empty), wav) == sizeof(empty), "WAV header write failed", 0);
    }
    char *args[] = {argv[0], "--rom", argv[1], "--frames", argv[3], "--data", argv[4], "--mute", NULL};
    int result = media_player_main(getenv("RECOMP_AUDIO_MUTE") ? 8 : 7, args, capture);
    if (wav) {
        uint8_t header[44] = "RIFF\0\0\0\0WAVEfmt ";
        le32(header+4, wav_bytes+36); le32(header+16, 16);
        header[20] = 1; header[22] = 2;
        le32(header+24, wav_rate); le32(header+28, wav_rate*4);
        header[32] = 4; header[34] = 16;
        memcpy(header+36, "data", 4); le32(header+40, wav_bytes);
        recomp_game_require(!fseek(wav, 0, SEEK_SET) && fwrite(header, 1, 44, wav) == 44,
                            "WAV header finalize failed", 0);
        recomp_game_require(!fclose(wav), "WAV capture close failed", 0);
    }
    return result;
}
