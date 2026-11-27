#pragma once
#include "recomp_audio.h"

static const RecompAudioTaskProfile audio_profile = {
    .rsp = &game_audio_rsp_program,
    .descriptor = {0x80007e80, 0xd0, 0x80009380, 0x1000, 0x80097880, 0x800},
    .fingerprint_bytes = {0xd0, 0xe20, 0x2c0},
    .sha256 = {"5759e9bb21f2e504bfbb3e5b75173cb81aa50c60b19e77bcee1d0f6fc34e8fa4",
               "14e3b245e8cd4e0bdf4cbb864af82d1ccba6d3d3ff33f73cb52c5821a76f30fc",
               "79bb2a3e8555019bc4b229f0c218f1e329ca5711bfa55c7c7f992facf2e12c83"},
};
