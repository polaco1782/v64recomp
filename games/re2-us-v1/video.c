/* Native RE2 bilinear resize, adapted from Mupen64Plus RSP HLE re2.c.
 * Copyright (C) 2016 Gilles Siberlin. GPL-2.0-or-later.
 * Upstream revision: 8a7a472a7172eb2c8725b305eae26818ed7b51a2.
 * This program is free software, without warranty; see LICENSES and the
 * bundled Mupen64Plus-rsp-hle license for redistribution terms.
 */
#include "video.h"
#include "game.h"
#include "video_dsp.h"
#include <openssl/evp.h>
#include <string.h>

static bool hash_matches(Runtime *rt, uint32_t address, size_t size, const char *expected) {
    uint8_t image[4096], digest[32];
    unsigned count = 0;
    recomp_game_require(size <= sizeof(image), "RE2 video fingerprint size", address);
    for (size_t i = 0; i < size; i++)
        image[i] = (uint8_t)recomp_load(rt, address + (uint32_t)i, 1);
    if (!EVP_Digest(image, size, digest, &count, EVP_sha256(), NULL) || count != 32)
        return false;
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < 32; i++)
        if (hex[digest[i] >> 4] != expected[i * 2] || hex[digest[i] & 15] != expected[i * 2 + 1])
            return false;
    return true;
}
static struct re2_rsp_resize resize_plan(Runtime *rt, const RecompTask *task) {
    uint8_t data[128];
    for (unsigned i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)recomp_load(rt, task->words[6] + i, 1);
    struct re2_rsp_resize r;
    recomp_game_require(re2_rsp_resize_read(data, &r), "unsupported RE2 resize parameters", task->words[6]);
    recomp_game_require(recomp_ram_address(rt, (uint32_t)recomp_load(rt, task->words[6], 4), 1) == r.src &&
                            recomp_ram_address(rt, (uint32_t)recomp_load(rt, task->words[6] + 4, 4), 1) == r.dst,
                        "RE2 resize pointers do not resolve to the DSP RAM window", task->words[6]);
    /* Validate the entire plan before any output. The RSP stages two complete
     * source rows, then reads its own output DMEM for the edge beyond pixel 319. */
    uint64_t last_y = (uint64_t)r.y0 + (uint64_t)(r.rows - 1) * r.y_ratio;
    recomp_game_require(last_y <= UINT32_MAX, "RE2 resize Y coordinate overflow", r.src);
    uint64_t source_end = (uint64_t)r.src + (last_y >> 16) * RE2_RSP_SRC_ROW + 2 * RE2_RSP_SRC_ROW;
    uint64_t output_end = (uint64_t)r.dst + (uint64_t)(r.rows - 1) * r.stride + 2 * r.width;
    recomp_game_require(source_end <= rt->rdram_size && output_end <= rt->rdram_size &&
                            (source_end <= r.dst || output_end <= r.src),
                        "RE2 resize buffers overlap or exceed RAM", r.dst);
    return r;
}
static struct re2_rsp_decode decode_plan(Runtime *rt, const RecompTask *task) {
    uint8_t data[128];
    for (unsigned i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)recomp_load(rt, task->words[6] + i, 1);
    struct re2_rsp_decode d;
    recomp_game_require(re2_rsp_decode_read(data, &d), "unsupported RE2 movie conversion parameters", task->words[6]);
    const uint32_t pointers[] = {d.y, d.cb, d.cr, d.dst};
    for (unsigned i = 0; i < 4; i++)
        recomp_game_require(recomp_ram_address(rt, (uint32_t)recomp_load(rt, task->words[6] + i * 4, 4), 1) ==
                                pointers[i],
                            "RE2 movie pointer does not resolve to DSP RAM", task->words[6]);
    uint64_t y_bytes = (uint64_t)d.pairs * 2 * d.width;
    uint64_t c_bytes = (uint64_t)(d.pairs - 1) * (d.width / 2) + d.width;
    uint64_t out_bytes = (uint64_t)(d.pairs - 1) * d.step + d.step / 2 + d.width * 4;
    recomp_game_require(y_bytes <= rt->rdram_size - d.y && c_bytes <= rt->rdram_size - d.cb &&
                            c_bytes <= rt->rdram_size - d.cr && out_bytes <= rt->rdram_size - d.dst,
                        "RE2 movie conversion spans exceed RAM", d.dst);
    const uint64_t input_bytes[] = {y_bytes, c_bytes, c_bytes};
    for (unsigned i = 0; i < 3; i++)
        recomp_game_require((uint64_t)pointers[i] + input_bytes[i] <= d.dst ||
                                (uint64_t)d.dst + out_bytes <= pointers[i],
                            "RE2 movie conversion buffers overlap", d.dst);
    return d;
}
static struct re2_rsp_fill fill_plan(Runtime *rt, const RecompTask *task) {
    uint8_t data[128];
    for (unsigned i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)recomp_load(rt, task->words[6] + i, 1);
    struct re2_rsp_fill f;
    recomp_game_require(re2_rsp_fill_read(data, &f), "unsupported RE2 frame blend parameters", task->words[6]);
    recomp_game_require(recomp_ram_address(rt, (uint32_t)recomp_load(rt, task->words[6], 4), 1) == f.src &&
                            recomp_ram_address(rt, (uint32_t)recomp_load(rt, task->words[6] + 4, 4), 1) == f.dst,
                        "RE2 blend pointer does not resolve to DSP RAM", task->words[6]);
    uint64_t bytes = (uint64_t)(f.iters - 1) * f.advance + (uint64_t)(f.rows - 1) * (f.len + f.skip) + f.len;
    recomp_game_require(bytes <= rt->rdram_size - f.src && bytes <= rt->rdram_size - f.dst &&
                            (f.src == f.dst || (uint64_t)f.src + bytes <= f.dst || (uint64_t)f.dst + bytes <= f.src),
                        "RE2 blend spans exceed RAM or partially overlap", f.dst);
    return f;
}
bool re2_video_validate(Runtime *rt, void *opaque, const RecompTask *task) {
    (void)opaque;
    bool resize = task->words[4] == 0x80031460;
    bool decode = task->words[4] == 0x80184470;
    bool fill = task->words[4] == 0x80184970;
    if (task->words[0] != 1 || (!resize && !decode && !fill))
        return false;
    recomp_game_require(!task->resumed && task->words[1] == RECOMP_TASK_LOADABLE && task->words[2] == 0x80018a80 &&
                            task->words[3] == 0xd0 && task->words[5] == 0x1000 &&
                            task->words[6] == (resize   ? 0x80134398
                                               : decode ? 0x80134298
                                                        : 0x80134318) &&
                            task->words[7] == 0x80,
                        "RE2 video task descriptor mismatch", task->guest_address);
    for (unsigned i = 8; i < 16; i++)
        recomp_game_require(task->words[i] == 0, "RE2 video unexpected task buffer", task->guest_address);
    const uint32_t constants[][6] = {
        {0xffff7fff, 0x00030004, 0x000100f8, 0x40000008, 0x00800007, 0x800000ff},
        {0xb3755b69, 0x2c0de2d1, 0x950b1000, 0x02000080, 0x40007c00, 0x00017f80},
    };
    for (unsigned i = 0; !fill && i < 6; i++)
        recomp_game_require(recomp_load(rt, task->words[6] + 0x40 + i * 4, 4) == constants[decode][i],
                            "RE2 video vector constants mismatch", task->words[6]);
    const uint32_t blend_constants[] = {0xf80007c0, 0x003e0001, 0x80000800, 0x00400002};
    for (unsigned i = 0; fill && i < 4; i++)
        recomp_game_require(recomp_load(rt, task->words[6] + 0x20 + i * 4, 4) == blend_constants[i],
                            "RE2 blend vector constants mismatch", task->words[6]);
    recomp_game_require(
        hash_matches(rt, task->words[2], 0xd0, "5759e9bb21f2e504bfbb3e5b75173cb81aa50c60b19e77bcee1d0f6fc34e8fa4") &&
            /* Decode is a complete 0x500-byte RSP program ending with its DMA
         * helpers. Fill starts at 0x80184970; CPU-owned state begins at
         * 0x80184b20. The SDK's fixed 0x1000 size extends into both. */
            hash_matches(rt, task->words[4],
                         resize   ? 0x1000
                         : decode ? 0x500
                                  : 0x150,
                         resize   ? "1ab144cb08ce5131b3d220bd9f87f451a19eebd0e4c2935fb817e3da270154ae"
                         : decode ? "ad1de98206b16f50105f868e9e6574d6281ecc5f8871573e63cd6d0f7f9ad7fb"
                                  : "c7d45011671405a390c3c6ec9ac9d2633d4d8a3e095110a36e46610e3d19dec7"),
        "RE2 video microcode fingerprint mismatch", task->words[4]);
    if (resize)
        (void)resize_plan(rt, task);
    else if (decode)
        (void)decode_plan(rt, task);
    else
        (void)fill_plan(rt, task);
    return true;
}
bool re2_video_submit(Runtime *rt, const RecompTask *task) {
    if (!re2_video_validate(rt, NULL, task))
        return false;
    if (task->words[4] == 0x80184970) {
        struct re2_rsp_fill f = fill_plan(rt, task);
        uint8_t source[2048], destination[2048];
        for (uint32_t iteration = 0; iteration < f.iters; iteration++) {
            for (uint32_t row = 0; row < f.rows; row++) {
                uint32_t offset = iteration * f.advance + row * (f.len + f.skip);
                for (unsigned i = 0; i < f.len; i++) {
                    source[i] = (uint8_t)recomp_load(rt, f.src + offset + i, 1);
                    destination[i] = (uint8_t)recomp_load(rt, f.dst + offset + i, 1);
                }
                re2_rsp_fill_row(source, destination, f.len);
                recomp_copy_to_ram(rt, f.dst + offset, destination, f.len);
            }
        }
        goto complete;
    }
    if (task->words[4] == 0x80184470) {
        struct re2_rsp_decode d = decode_plan(rt, task);
        uint8_t y[480], cb[240], cr[240], out0[960], out1[960];
        for (uint32_t pair = 0; pair < d.pairs; pair++) {
            for (unsigned i = 0; i < d.width * 2; i++)
                y[i] = (uint8_t)recomp_load(rt, d.y + pair * d.width * 2 + i, 1);
            for (unsigned i = 0; i < d.width; i++) {
                cb[i] = (uint8_t)recomp_load(rt, d.cb + pair * (d.width / 2) + i, 1);
                cr[i] = (uint8_t)recomp_load(rt, d.cr + pair * (d.width / 2) + i, 1);
            }
            re2_rsp_decode_pair(&d, y, cb, cr, out0, out1);
            recomp_copy_to_ram(rt, d.dst + pair * d.step, out0, d.width * 4);
            recomp_copy_to_ram(rt, d.dst + pair * d.step + d.step / 2, out1, d.width * 4);
        }
        goto complete;
    }
    struct re2_rsp_resize r = resize_plan(rt, task);
    uint8_t dmem[4096] = {0};
    struct re2_rsp_dither state;
    re2_rsp_resize_begin(&state, task->words[8], task->words[9]);
    for (uint32_t row = 0; row < r.rows; row++) {
        uint32_t source = r.src + ((r.y0 + row * r.y_ratio) >> 16) * RE2_RSP_SRC_ROW;
        for (unsigned i = 0; i < 2 * RE2_RSP_SRC_ROW; i++)
            dmem[i] = (uint8_t)recomp_load(rt, source + i, 1);
        re2_rsp_resize_row(dmem, &r, &state, row);
        recomp_copy_to_ram(rt, r.dst + row * r.stride, dmem + RE2_RSP_OUT_BASE, r.width * 2);
    }
complete:
    recomp_game_require(recomp_task_complete(rt, task->token, RECOMP_SP_DONE) &&
                            recomp_task_finish_without_dp(rt, task->token),
                        "RE2 video completion rejected", task->guest_address);
    return true;
}
