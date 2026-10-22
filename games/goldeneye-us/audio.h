#pragma once
#include "recomp_audio.h"

static const RecompAudioTaskProfile goldeneye_audio_task_profile = {
    .descriptor = {0x80020d90, 0xd0, 0x80022280, 0, 0x8005d020, 0x800},
    /* ucode_data_size includes DMEM BSS; only 0x2c0 bytes are initialized. */
    .fingerprint_bytes = {0xd0, 0x800, 0x2c0},
    .sha256 =
        {
            "5759e9bb21f2e504bfbb3e5b75173cb81aa50c60b19e77bcee1d0f6fc34e8fa4",
            "fd6d1c08eb9de2bf4c1694010605177329ea34ced98ce5b76c02c93e47968c47",
            "e6afb20043c940e2e85a9ecb1fb064b183e9389eb8e3b1b260726d2477b28c7a",
        },
};
