#pragma once
#include "recomp_audio.h"
/* MusyX v1: the task size counts 0xa10-byte subframe descriptors.
 * Data after the 0x230 constant bytes includes CPU-owned state at
 * 0x80136d80 (set by 0x800b41fc), so it is not a constant fingerprint. */
static const RecompAudioTaskProfile re2_audio_profile = {
    .descriptor = {0x80018a80, 0xd0, 0x800b2700, 0x1000, 0x80136b50, 0x800},
    .fingerprint_bytes = {0xd0, 0x1000, 0x230},
    .sha256 =
        {
            "5759e9bb21f2e504bfbb3e5b75173cb81aa50c60b19e77bcee1d0f6fc34e8fa4",
            "8ea8bc9fb5fce653fc95238dd9b5c487c99375957b1793b2786a07e8f69cf4e8",
            "8567eb365b231fef6a2cd4311a1e16be640b78cfb211a8d68aced477aeb2406b",
        },
};
