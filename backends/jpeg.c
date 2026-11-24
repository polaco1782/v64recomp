#include "recomp_jpeg.h"
#include <string.h>

static bool physical_range(Runtime *rt, uint32_t address, uint64_t bytes) {
    return !(address & 7) && address < rt->rdram_size && bytes <= rt->rdram_size - address;
}

bool recomp_jpeg_submit(Runtime *rt, const RecompTask *task) {
    const RecompJpegProfile *profile = &game_jpeg_profile;
    if (rt->rdram_size > 0x1000000 || task->words[0] != 4 || task->words[1] || task->resumed || task->words[12] % 8 ||
        task->words[13] != 0x20 || memcmp(task->words + 2, profile->descriptor, sizeof(profile->descriptor)))
        return false;
    uint32_t words[16];
    memcpy(words, task->words, sizeof(words));
    for (unsigned i = 2; i <= 6; i += 2)
        words[i] = recomp_ram_address(rt, words[i], words[i + 1]);
    for (size_t i = 0; i < profile->data_size; i++)
        if (rt->rdram[(words[6] + i) ^ 3] != profile->data[i])
            return false;
    uint32_t header = task->words[12];
    words[12] = recomp_ram_address(rt, header, words[13]);
    if (words[15])
        words[14] = recomp_ram_address(rt, words[14], words[15]);
    uint32_t buffer = recomp_load32(rt, header);
    uint32_t count = recomp_load32(rt, header + 4);
    uint32_t mode = recomp_load32(rt, header + 8);
    if ((mode != 0 && mode != 2) || !count || !physical_range(rt, buffer, (uint64_t)count * (mode + 4) * 128))
        return false;
    for (unsigned i = 0; i < 3; i++)
        if (!physical_range(rt, recomp_load32(rt, header + 12 + i * 4), 128))
            return false;

    RecompRspState rsp;
    recomp_rsp_init(&rsp, rt->rdram, rt->rdram_size, profile->program);
    if (!recomp_rsp_start(&rsp, words))
        recomp_fail(rsp.error, rsp.error_address);
    bool completed = (rsp.cp0[4] & RECOMP_RSP_STATUS_TASKDONE) && !rsp.rdp_count;
    recomp_rsp_destroy(&rsp);
    return completed;
}
