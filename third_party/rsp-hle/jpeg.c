/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *   Mupen64plus-rsp-hle - jpeg.c                                          *
 *   Mupen64Plus homepage: https://mupen64plus.org/                        *
 *   Copyright (C) 2012 Bobby Smiles                                       *
 *   Copyright (C) 2009 Richard Goedeken                                   *
 *   Copyright (C) 2002 Hacktarux                                          *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.          *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#include "arithmetics.h"
#include "hle_external.h"
#include "hle_internal.h"
#include "memory.h"

#define SUBBLOCK_SIZE 64

/* helper functions */
static uint16_t clamp_RGBA_component(int16_t x);

/* pixel conversion & formatting */
static uint16_t GetRGBA(int16_t y, int16_t u, int16_t v);

/* tile line emitters */
static void EmitRGBATileLine(struct hle_t* hle, const int16_t *y, const int16_t *u, uint32_t address);

/* macroblocks operations */
static void decode_macroblock_std(int16_t *macroblock,
                                  unsigned int subblock_count,
                                  const int16_t qtables[3][SUBBLOCK_SIZE]);
static void EmitTilesMode0(struct hle_t* hle, const int16_t *macroblock, uint32_t address);
static void EmitTilesMode2(struct hle_t* hle, const int16_t *macroblock, uint32_t address);

/* subblocks operations */
static void ZigZagSubBlock(int16_t *dst, const int16_t *src);
static void ReorderSubBlock(int16_t *dst, const int16_t *src, const unsigned int *table);
static void MultSubBlocks(int16_t *dst, const int16_t *src1, const int16_t *src2, unsigned int shift);
static void InverseDCT1D(const float *const x, float *dst, unsigned int stride);
static void InverseDCTSubBlock(int16_t *dst, const int16_t *src);

/* zig-zag indices */
static const unsigned int ZIGZAG_TABLE[SUBBLOCK_SIZE] = {
     0,  1,  5,  6, 14, 15, 27, 28,
     2,  4,  7, 13, 16, 26, 29, 42,
     3,  8, 12, 17, 25, 30, 41, 43,
     9, 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63
};

/* IDCT related constants
 * Cn = alpha * cos(n * PI / 16) (alpha is chosen such as C4 = 1) */
static const float IDCT_C3 = 1.175875602f;
static const float IDCT_C6 = 0.541196100f;
static const float IDCT_K[10] = {
     0.765366865f,   /*  C2-C6         */
    -1.847759065f,   /* -C2-C6         */
    -0.390180644f,   /*  C5-C3         */
    -1.961570561f,   /* -C5-C3         */
     1.501321110f,   /*  C1+C3-C5-C7   */
     2.053119869f,   /*  C1+C3-C5+C7   */
     3.072711027f,   /*  C1+C3+C5-C7   */
     0.298631336f,   /* -C1+C3+C5-C7   */
    -0.899976223f,   /*  C7-C3         */
    -2.562915448f    /* -C1-C3         */
};


/* PS JPEG microcode used by the Zelda titles. */
void jpeg_decode_PS(struct hle_t* hle)
{
    int16_t qtables[3][SUBBLOCK_SIZE];
    unsigned int mb;
    uint32_t address;
    uint32_t macroblock_count;
    uint32_t mode;
    uint32_t qtableY_ptr;
    uint32_t qtableU_ptr;
    uint32_t qtableV_ptr;
    unsigned int subblock_count;
    unsigned int macroblock_size;
    /* macroblock contains at most 6 subblocks */
    int16_t macroblock[6 * SUBBLOCK_SIZE];
    uint32_t data_ptr;

    if (*dmem_u32(hle, TASK_FLAGS) & 0x1) {
        HleWarnMessage(hle->user_defined,
                       "jpeg_decode_PS: task yielding not implemented");
        goto done;
    }

    data_ptr = *dmem_u32(hle, TASK_DATA_PTR);
    address          = *dram_u32(hle, data_ptr);
    macroblock_count = *dram_u32(hle, data_ptr + 4);
    mode             = *dram_u32(hle, data_ptr + 8);
    qtableY_ptr      = *dram_u32(hle, data_ptr + 12);
    qtableU_ptr      = *dram_u32(hle, data_ptr + 16);
    qtableV_ptr      = *dram_u32(hle, data_ptr + 20);

    HleVerboseMessage(hle->user_defined,
                      "jpeg_decode_PS: *buffer=%x, #MB=%d, mode=%d, *Qy=%x, *Qu=%x, *Qv=%x",
                      address,
                      macroblock_count,
                      mode,
                      qtableY_ptr,
                      qtableU_ptr,
                      qtableV_ptr);

    if (mode != 0 && mode != 2) {
        HleWarnMessage(hle->user_defined,
                       "jpeg_decode_PS: invalid mode %d", mode);
        goto done;
    }

    subblock_count = mode + 4;
    macroblock_size = subblock_count * SUBBLOCK_SIZE;

    dram_load_u16(hle, (uint16_t *)qtables[0], qtableY_ptr, SUBBLOCK_SIZE);
    dram_load_u16(hle, (uint16_t *)qtables[1], qtableU_ptr, SUBBLOCK_SIZE);
    dram_load_u16(hle, (uint16_t *)qtables[2], qtableV_ptr, SUBBLOCK_SIZE);

    for (mb = 0; mb < macroblock_count; ++mb) {
        dram_load_u16(hle, (uint16_t *)macroblock, address, macroblock_size);
        decode_macroblock_std(macroblock, subblock_count, (const int16_t (*)[SUBBLOCK_SIZE])qtables);

        if (mode == 0)
            EmitTilesMode0(hle, macroblock, address);
        else
            EmitTilesMode2(hle, macroblock, address);

        address += 2 * macroblock_size;
    }
done:
    rsp_break(hle, SP_STATUS_TASKDONE);
}

static uint16_t clamp_RGBA_component(int16_t x)
{
    if (x > 0xff0)
        x = 0xff0;
    else if (x < 0)
        x = 0;
    return (x & 0xf80);
}

static uint16_t GetRGBA(int16_t y, int16_t u, int16_t v)
{
    const float fY = (float)y + 2048.0f;
    const float fU = (float)u;
    const float fV = (float)v;

    const uint16_t r = clamp_RGBA_component((int16_t)(fY               + 1.4025 * fV));
    const uint16_t g = clamp_RGBA_component((int16_t)(fY - 0.3443 * fU - 0.7144 * fV));
    const uint16_t b = clamp_RGBA_component((int16_t)(fY + 1.7729 * fU));

    return (r << 4) | (g >> 1) | (b >> 6) | 1;
}

static void EmitRGBATileLine(struct hle_t* hle, const int16_t *y, const int16_t *u, uint32_t address)
{
    uint16_t rgba[16];

    const int16_t *const v  = u + SUBBLOCK_SIZE;
    const int16_t *const y2 = y + SUBBLOCK_SIZE;

    rgba[0]  = GetRGBA(y[0],  u[0], v[0]);
    rgba[1]  = GetRGBA(y[1],  u[0], v[0]);
    rgba[2]  = GetRGBA(y[2],  u[1], v[1]);
    rgba[3]  = GetRGBA(y[3],  u[1], v[1]);
    rgba[4]  = GetRGBA(y[4],  u[2], v[2]);
    rgba[5]  = GetRGBA(y[5],  u[2], v[2]);
    rgba[6]  = GetRGBA(y[6],  u[3], v[3]);
    rgba[7]  = GetRGBA(y[7],  u[3], v[3]);
    rgba[8]  = GetRGBA(y2[0], u[4], v[4]);
    rgba[9]  = GetRGBA(y2[1], u[4], v[4]);
    rgba[10] = GetRGBA(y2[2], u[5], v[5]);
    rgba[11] = GetRGBA(y2[3], u[5], v[5]);
    rgba[12] = GetRGBA(y2[4], u[6], v[6]);
    rgba[13] = GetRGBA(y2[5], u[6], v[6]);
    rgba[14] = GetRGBA(y2[6], u[7], v[7]);
    rgba[15] = GetRGBA(y2[7], u[7], v[7]);

    dram_store_u16(hle, rgba, address, 16);
}

static void EmitTilesMode0(struct hle_t* hle, const int16_t *macroblock, uint32_t address)
{
    unsigned int i;

    unsigned int y_offset = 0;
    unsigned int u_offset = 2 * SUBBLOCK_SIZE;

    for (i = 0; i < 8; ++i) {
        EmitRGBATileLine(hle, &macroblock[y_offset], &macroblock[u_offset], address);

        y_offset += 8;
        u_offset += 8;
        address += 32;
    }
}

static void EmitTilesMode2(struct hle_t* hle, const int16_t *macroblock, uint32_t address)
{
    unsigned int i;

    unsigned int y_offset = 0;
    unsigned int u_offset = 4 * SUBBLOCK_SIZE;

    for (i = 0; i < 8; ++i) {
        EmitRGBATileLine(hle, &macroblock[y_offset],     &macroblock[u_offset], address);
        EmitRGBATileLine(hle, &macroblock[y_offset + 8], &macroblock[u_offset], address + 32);

        y_offset += (i == 3) ? SUBBLOCK_SIZE + 16 : 16;
        u_offset += 8;
        address += 64;
    }
}

static void decode_macroblock_std(int16_t *macroblock,
                                  unsigned int subblock_count,
                                  const int16_t qtables[3][SUBBLOCK_SIZE])
{
    unsigned int sb;
    unsigned int q = 0;

    for (sb = 0; sb < subblock_count; ++sb) {
        int16_t tmp_sb[SUBBLOCK_SIZE];
        const int isChromaSubBlock = (subblock_count - sb <= 2);

        if (isChromaSubBlock)
            ++q;

        MultSubBlocks(macroblock, macroblock, qtables[q], 4);
        ZigZagSubBlock(tmp_sb, macroblock);
        InverseDCTSubBlock(macroblock, tmp_sb);

        macroblock += SUBBLOCK_SIZE;
    }
}

static void ZigZagSubBlock(int16_t *dst, const int16_t *src)
{
    ReorderSubBlock(dst, src, ZIGZAG_TABLE);
}

static void ReorderSubBlock(int16_t *dst, const int16_t *src, const unsigned int *table)
{
    unsigned int i;

    /* source and destination sublocks cannot overlap */
    assert((uintptr_t)dst + SUBBLOCK_SIZE * sizeof(*dst) <= (uintptr_t)src ||
           (uintptr_t)src + SUBBLOCK_SIZE * sizeof(*src) <= (uintptr_t)dst);

    for (i = 0; i < SUBBLOCK_SIZE; ++i)
        dst[i] = src[table[i]];
}

static void MultSubBlocks(int16_t *dst, const int16_t *src1, const int16_t *src2, unsigned int shift)
{
    unsigned int i;

    for (i = 0; i < SUBBLOCK_SIZE; ++i) {
        int32_t v = src1[i] * src2[i];
        dst[i] = (uint16_t)clamp_s16(v) << shift;
    }
}

/***************************************************************************
 * Fast 2D IDCT using separable formulation and normalization
 * Computations use single precision floats
 * Implementation based on Wikipedia :
 * http://fr.wikipedia.org/wiki/Transform%C3%A9e_en_cosinus_discr%C3%A8te
 **************************************************************************/
static void InverseDCT1D(const float *const x, float *dst, unsigned int stride)
{
    float e[4];
    float f[4];
    float x26, x1357, x15, x37, x17, x35;

    x15   = IDCT_K[2] * (x[1] + x[5]);
    x37   = IDCT_K[3] * (x[3] + x[7]);
    x17   = IDCT_K[8] * (x[1] + x[7]);
    x35   = IDCT_K[9] * (x[3] + x[5]);
    x1357 = IDCT_C3   * (x[1] + x[3] + x[5] + x[7]);
    x26   = IDCT_C6   * (x[2] + x[6]);

    f[0] = x[0] + x[4];
    f[1] = x[0] - x[4];
    f[2] = x26  + IDCT_K[0] * x[2];
    f[3] = x26  + IDCT_K[1] * x[6];

    e[0] = x1357 + x15 + IDCT_K[4] * x[1] + x17;
    e[1] = x1357 + x37 + IDCT_K[6] * x[3] + x35;
    e[2] = x1357 + x15 + IDCT_K[5] * x[5] + x35;
    e[3] = x1357 + x37 + IDCT_K[7] * x[7] + x17;

    *dst = f[0] + f[2] + e[0];
    dst += stride;
    *dst = f[1] + f[3] + e[1];
    dst += stride;
    *dst = f[1] - f[3] + e[2];
    dst += stride;
    *dst = f[0] - f[2] + e[3];
    dst += stride;
    *dst = f[0] - f[2] - e[3];
    dst += stride;
    *dst = f[1] - f[3] - e[2];
    dst += stride;
    *dst = f[1] + f[3] - e[1];
    dst += stride;
    *dst = f[0] + f[2] - e[0];
}

static void InverseDCTSubBlock(int16_t *dst, const int16_t *src)
{
    float x[8];
    float block[SUBBLOCK_SIZE];
    unsigned int i, j;

    /* idct 1d on rows (+transposition) */
    for (i = 0; i < 8; ++i) {
        for (j = 0; j < 8; ++j)
            x[j] = (float)src[i * 8 + j];

        InverseDCT1D(x, &block[i], 8);
    }

    /* idct 1d on columns (thanks to previous transposition) */
    for (i = 0; i < 8; ++i) {
        InverseDCT1D(&block[i * 8], x, 1);

        /* C4 = 1 normalization implies a division by 8 */
        for (j = 0; j < 8; ++j)
            dst[i + j * 8] = (int16_t)(int32_t)x[j] >> 3;
    }
}
