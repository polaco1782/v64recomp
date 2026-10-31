#ifndef MEDIA_CODEC_H
#define MEDIA_CODEC_H
#include "game.h"
#include <stdio.h>
typedef struct MediaCodec {
    Runtime rt;
    FILE *file;
    uint8_t input[65536];
    size_t available;
    uint64_t bytes, frames;
    int rate, channels, bitrate, samples;
    int16_t pcm[2304]; /* Always host-endian stereo, including mono input. */
} MediaCodec;
void media_codec_init(MediaCodec *, const RecompGameOptions *);
bool media_codec_open(MediaCodec *, const char *);
bool media_codec_restart(MediaCodec *);
/* 1 = decoded frame, 0 = skipped bytes, -1 = EOF, -2 = read error. */
int media_codec_step(MediaCodec *);
void media_codec_close(MediaCodec *);
#endif
