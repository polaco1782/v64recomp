/* SPDX-License-Identifier: GPL-2.0-or-later
 * Video interfaces adapted from Salsa64, revision recorded in video_dsp.c. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define RE2_RSP_SRC_ROW 960u
#define RE2_RSP_OUT_BASE 0x780u
struct re2_rsp_resize {
    uint32_t src, dst, width, rows, x_ratio, y_ratio, stride, x0, y0;
    int32_t thresh;
};
struct re2_rsp_dither {
    int32_t v[3][8], last[3];
};
bool re2_rsp_resize_read(const uint8_t *, struct re2_rsp_resize *);
void re2_rsp_resize_begin(struct re2_rsp_dither *, uint32_t, uint32_t);
void re2_rsp_resize_row(uint8_t *, const struct re2_rsp_resize *, struct re2_rsp_dither *, uint32_t);
struct re2_rsp_decode {
    uint32_t y, cb, cr, dst, width, pairs, step, k[5];
    int32_t zero, max;
};
bool re2_rsp_decode_read(const uint8_t *, struct re2_rsp_decode *);
void re2_rsp_decode_pair(const struct re2_rsp_decode *, const uint8_t *, const uint8_t *, const uint8_t *, uint8_t *,
                         uint8_t *);
struct re2_rsp_fill {
    uint32_t src, dst, len, rows, skip, iters, advance;
};
bool re2_rsp_fill_read(const uint8_t *, struct re2_rsp_fill *);
void re2_rsp_fill_row(const uint8_t *, uint8_t *, uint32_t);
