#pragma once
#include "recomp_audio.h"

/* The audio task builder at 0x8002ed08 fills boot, ABI1 text and DMEM pointers.
 * Its 0x800-byte DMEM request includes 0x2c0 bytes of initialized constants. */
static const RecompAudioTaskProfile doom64_audio_profile = {
    .rsp = &game_audio_rsp_program,
    .descriptor = {0x80049fe0, 0xd0, 0x8004c3e0, 0, 0x80062e50, 0x800},
    .fingerprint_bytes = {0xd0, 0x800, 0x2c0},
    .sha256 =
        {
            "5759e9bb21f2e504bfbb3e5b75173cb81aa50c60b19e77bcee1d0f6fc34e8fa4",
            "fd6d1c08eb9de2bf4c1694010605177329ea34ced98ce5b76c02c93e47968c47",
            "79bb2a3e8555019bc4b229f0c218f1e329ca5711bfa55c7c7f992facf2e12c83",
        },
};
