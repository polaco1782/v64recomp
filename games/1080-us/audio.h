#pragma once
#include "recomp_audio.h"

/* Exact first-task image after the original USO loader. NEAD signature at
 * ucode_data + 0x10 is 0x1f38122c; the shared HLE has a dedicated 1080 entry. */
static const RecompAudioTaskProfile snowboarding_audio_profile = {
    .descriptor = {0x800dc2c0, 0x1000, 0x800dc2c0, 0x1000, 0x80084b20, 0x2df},
    .fingerprint_bytes = {0x1000, 0x1000, 0x2df},
    .sha256 = {
        "34c21501b3ec562f9a9c583eb8412ad8aad9dc03f1424ac84bc17d8e7f43f74a",
        "34c21501b3ec562f9a9c583eb8412ad8aad9dc03f1424ac84bc17d8e7f43f74a",
        "0b2f6cb539a399201c4bcff461a7f8395ef0c4093f513893b80795ba6452667b",
    },
};
