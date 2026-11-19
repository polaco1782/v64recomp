#ifndef TUROK2_AUDIO_H
#define TUROK2_AUDIO_H
#include "recomp_audio.h"

/* Full task source spans match the original cartridge at 0xbcf30,
 * 0xbf520 and 0xc0b50 in the first captured audio task. Standard ABI1. */
static const RecompAudioTaskProfile turok2_audio_profile = {
    .descriptor = {0x800bc330, 0xd0, 0x800be920, 0x1000, 0x800bff50, 0x800},
    .fingerprint_bytes = {0xd0, 0x1000, 0x800},
    .sha256 =
        {
            "5759e9bb21f2e504bfbb3e5b75173cb81aa50c60b19e77bcee1d0f6fc34e8fa4",
            "1a4f760e4d6cd21a9d7df4d46a7fbbde10af36fdc8178d666afc67fbc782cee0",
            "1e396f4dfe0851479260e265d72d11cf506d2fc228c518ad4a72585cc017c220",
        },
};
#endif
