#pragma once
#include "recomp_audio.h"

static const RecompAudioTaskProfile sm64_audio_task_profile = {
    .descriptor = {0x8032b260, 0xd0, 0x8032c740, 0x800, 0x8033a2c0, 0x2c0},
    .fingerprint_bytes = {0xd0, 0x800, 0x2c0},
    .sha256 = {
        "589d793eda195c576c9d767e7a5e4b77352826f59669d20bdcc358a8ebc979a3",
        "fd6d1c08eb9de2bf4c1694010605177329ea34ced98ce5b76c02c93e47968c47",
        "79bb2a3e8555019bc4b229f0c218f1e329ca5711bfa55c7c7f992facf2e12c83",
    },
};
