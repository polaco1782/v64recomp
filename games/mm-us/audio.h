#pragma once
#include "recomp_audio.h"

static const RecompAudioTaskProfile mm_audio_profile = {
    .rsp = &game_audio_rsp_program,
    .descriptor = {0x801aaab0, 0x1000, 0x801aaab0, 0x1000, 0x801e1e80, 0x2df},
    .fingerprint_bytes = {0x1000, 0x1000, 0x2df},
    .sha256 =
        {
            "b19b603dd2c2c37ac28734648a5acfb39712caa8b154c46336c8ea1e1bc0e8d6",
            "b19b603dd2c2c37ac28734648a5acfb39712caa8b154c46336c8ea1e1bc0e8d6",
            "292610456adc01cdc36a70975dcc17c0f3cb3a8afb61c90a6536b85b3755b542",
        },
};
