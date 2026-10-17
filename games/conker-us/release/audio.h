#ifndef CONKER_AUDIO_H
#define CONKER_AUDIO_H
#include "recomp_audio.h"
/* OSTask at 0x80078108 produced by 0x100095a0; initialized bytes match the
 * verified cartridge at the corresponding physical offsets. Text size zero is
 * the SDK's default 4 KiB IMEM load. Data includes only initialized DMEM. */
static const RecompAudioTaskProfile conker_audio_task_profile = {
    .descriptor = {0x100290d0, 0xd0, 0x100291a0, 0, 0x8002c960, 0x800},
    .fingerprint_bytes = {0xd0, 0x1000, 0x800},
    .sha256 =
        {
            "5759e9bb21f2e504bfbb3e5b75173cb81aa50c60b19e77bcee1d0f6fc34e8fa4",
            "44ec0f79408a989382e88448b65969e5af9ebd89c255165814fadcace8174bd8",
            "ecc1749960422f2e9c7fe9eb3b60bf1bc6ef4ed913ededee2d2c2d04fe073b01",
        },
};
#endif
