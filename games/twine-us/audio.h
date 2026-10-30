#pragma once
#include "recomp_audio.h"
/* MusyX v1. The list length counts 0xa10-byte subframes, not commands.
 * The 0x230-byte constant data span matches RE2's verified MusyX v1 table;
 * the remainder includes writable state and is excluded from recognition. */
static const RecompAudioTaskProfile twine_audio_profile = {
    .descriptor = {0x800ac110, 0xd0, 0x800ad570, 0x1000, 0x800d1b30, 0x800},
    .fingerprint_bytes = {0xd0, 0x1000, 0x230},
    .sha256 = {
        "5759e9bb21f2e504bfbb3e5b75173cb81aa50c60b19e77bcee1d0f6fc34e8fa4",
        "b9f980b93cc0ca27646e67921f65af8ae5b50166aa5077e9f5d4099ffde1da2e",
        "8567eb365b231fef6a2cd4311a1e16be640b78cfb211a8d68aced477aeb2406b",
    },
};
