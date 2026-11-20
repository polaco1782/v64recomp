#pragma once
#include "recomp_jpeg.h"

static const RecompJpegProfile mm_jpeg_profile = {
    .descriptor = {0x800969c0, 0x160, 0x801ad370, 0x1000, 0x801e3f40, 0x800},
    .fingerprint_bytes = {0x160, 0xaf0, 0x60},
    .sha256 =
        {
            "f3305858f15a8c77d1fb03ea6331164a2ed18de5254a4cdfc677950b4e91ba1f",
            "0f56b7fd8dbe334877b26a0bba5e7e536ed0c21ff898841e77af6cdf30989b9e",
            "e961dab96ea15e04f8acf3d8185a1e6e6b66fd9094ee3f22fc3db84ef2503f72",
        },
};
