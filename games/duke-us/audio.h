#pragma once
#include "recomp_audio.h"

static const RecompAudioTaskProfile duke_audio_task_profile = {
    .descriptor = {0x800d13c0, 0xd0, 0x800d2890, 0x0, 0x8010a9c0, 0x800},
    /* Task data size includes DMEM BSS; 0x2c0 bytes are initialized. */
    .fingerprint_bytes = {0xd0, 0x800, 0x2c0},
    .sha256 = {
        "5759e9bb21f2e504bfbb3e5b75173cb81aa50c60b19e77bcee1d0f6fc34e8fa4",
        "fd6d1c08eb9de2bf4c1694010605177329ea34ced98ce5b76c02c93e47968c47",
        "79bb2a3e8555019bc4b229f0c218f1e329ca5711bfa55c7c7f992facf2e12c83",
    },
};
