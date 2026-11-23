#ifndef TUROK3_AUDIO_H
#define TUROK3_AUDIO_H
#include "recomp_audio.h"

/* The task builder at 0x243ca8 sets the boot, ABI1 text and data descriptors.
 * The 0x2c0-byte ABI1 DMEM constant pool matches Turok 2 byte for byte. */
static const RecompAudioTaskProfile turok3_audio_profile = {
    .rsp = &game_audio_rsp_program,
    .descriptor = {0x801107e0, 0xd0, 0x80112dd0, 0x1000, 0x80114400, 0x800},
    .fingerprint_bytes = {0xd0, 0x1000, 0x2c0},
    .sha256 =
        {
            "5759e9bb21f2e504bfbb3e5b75173cb81aa50c60b19e77bcee1d0f6fc34e8fa4",
            "1a4f760e4d6cd21a9d7df4d46a7fbbde10af36fdc8178d666afc67fbc782cee0",
            "79bb2a3e8555019bc4b229f0c218f1e329ca5711bfa55c7c7f992facf2e12c83",
        },
};
#endif
