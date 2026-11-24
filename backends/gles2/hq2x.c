/*
 * Copyright (c) 2014 Clément Bœsch
 *
 * This file is part of FFmpeg.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
/* HQ2x by Maxim Stepin. The corner rules come from FFmpeg's ISC implementation.
 * Use vnes's integer color conversion and its byte-domain Y/U/V/A thresholds. */
#include "hq2x.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t rgba;
    uint8_t y, u, v, a;
} Pixel;

static Pixel sample(const uint8_t *p) {
    int y = (299 * p[0] + 587 * p[1] + 114 * p[2]) / 1000;
    Pixel color = {.y = (uint8_t)y,
                   .u = (uint8_t)(128 + (p[2] - y) * 492 / 1000),
                   .v = (uint8_t)(128 + (p[0] - y) * 877 / 1000),
                   .a = p[3]};
    memcpy(&color.rgba, p, 4);
    return color;
}

static bool different(Pixel a, Pixel b) {
    return abs(a.y - b.y) > 48 || abs(a.u - b.u) > 7 || abs(a.v - b.v) > 6 || abs(a.a - b.a) > 80;
}

// Accumulate alternating byte lanes independently, including alpha on either host byte order.
static uint32_t blend(Pixel a, unsigned wa, Pixel b, unsigned wb, Pixel c, unsigned wc, unsigned shift) {
    const uint32_t lanes = 0x00ff00ff;
    uint32_t lo = (a.rgba & lanes) * wa + (b.rgba & lanes) * wb + (c.rgba & lanes) * wc;
    uint32_t hi = ((a.rgba >> 8) & lanes) * wa + ((b.rgba >> 8) & lanes) * wb + ((c.rgba >> 8) & lanes) * wc;
    return ((lo >> shift) & lanes) | (((hi >> shift) & lanes) << 8);
}

// The eight mask bits are the neighbors in row-major order, skipping the center.
#define MATCH(bits, value) ((mask & (bits)) == (value))
static uint32_t corner(unsigned mask, Pixel diagonal, Pixel north, Pixel west, Pixel center, Pixel east, Pixel south) {
    if ((MATCH(0xbf, 0x37) || MATCH(0xdb, 0x13)) && different(north, east))
        return blend(center, 3, west, 1, center, 0, 2);
    if ((MATCH(0xdb, 0x49) || MATCH(0xef, 0x6d)) && different(south, west))
        return blend(center, 3, north, 1, center, 0, 2);
    if ((MATCH(0x0b, 0x0b) || MATCH(0xfe, 0x4a) || MATCH(0xfe, 0x1a)) && different(west, north))
        return center.rgba;
    if ((MATCH(0x6f, 0x2a) || MATCH(0x5b, 0x0a) || MATCH(0xbf, 0x3a) || MATCH(0xdf, 0x5a) || MATCH(0x9f, 0x8a) ||
         MATCH(0xcf, 0x8a) || MATCH(0xef, 0x4e) || MATCH(0x3f, 0x0e) || MATCH(0xfb, 0x5a) || MATCH(0xbb, 0x8a) ||
         MATCH(0x7f, 0x5a) || MATCH(0xaf, 0x8a) || MATCH(0xeb, 0x8a)) &&
        different(west, north))
        return blend(center, 3, diagonal, 1, center, 0, 2);
    if (MATCH(0x0b, 0x08))
        return blend(center, 2, diagonal, 1, north, 1, 2);
    if (MATCH(0x0b, 0x02))
        return blend(center, 2, diagonal, 1, west, 1, 2);
    if (MATCH(0x2f, 0x2f))
        return blend(center, 14, west, 1, north, 1, 4);
    if (MATCH(0xbf, 0x37) || MATCH(0xdb, 0x13))
        return blend(center, 5, north, 2, west, 1, 3);
    if (MATCH(0xdb, 0x49) || MATCH(0xef, 0x6d))
        return blend(center, 5, west, 2, north, 1, 3);
    if (MATCH(0x1b, 0x03) || MATCH(0x4f, 0x43) || MATCH(0x8b, 0x83) || MATCH(0x6b, 0x43))
        return blend(center, 3, west, 1, center, 0, 2);
    if (MATCH(0x4b, 0x09) || MATCH(0x8b, 0x89) || MATCH(0x1f, 0x19) || MATCH(0x3b, 0x19))
        return blend(center, 3, north, 1, center, 0, 2);
    if (MATCH(0x7e, 0x2a) || MATCH(0xef, 0xab) || MATCH(0xbf, 0x8f) || MATCH(0x7e, 0x0e))
        return blend(center, 2, west, 3, north, 3, 3);
    if (MATCH(0xfb, 0x6a) || MATCH(0x6f, 0x6e) || MATCH(0x3f, 0x3e) || MATCH(0xfb, 0xfa) || MATCH(0xdf, 0xde) ||
        MATCH(0xdf, 0x1e))
        return blend(center, 3, diagonal, 1, center, 0, 2);
    if (MATCH(0x0a, 0x00) || MATCH(0x4f, 0x4b) || MATCH(0x9f, 0x1b) || MATCH(0x2f, 0x0b) || MATCH(0xbe, 0x0a) ||
        MATCH(0xee, 0x0a) || MATCH(0x7e, 0x0a) || MATCH(0xeb, 0x4b) || MATCH(0x3b, 0x1b))
        return blend(center, 2, west, 1, north, 1, 2);
    return blend(center, 6, west, 1, north, 1, 3);
}
#undef MATCH

static unsigned flip_x(unsigned mask) {
    return ((mask & 0x21) << 2) | ((mask & 0x84) >> 2) | ((mask & 0x08) << 1) | ((mask & 0x10) >> 1) | (mask & 0x42);
}

static unsigned flip_y(unsigned mask) {
    return ((mask & 0x07) << 5) | ((mask & 0xe0) >> 5) | (mask & 0x18);
}

void gles2_hq2x(const uint8_t *rgba, unsigned width, unsigned height, bool wrap_x, bool wrap_y, uint32_t *out) {
    for (unsigned y = 0; y < height; ++y) {
        unsigned above = y ? y - 1 : (wrap_y ? height - 1 : 0);
        unsigned below = y + 1 < height ? y + 1 : (wrap_y ? 0 : y);
        const uint8_t *rows[] = {rgba + above * width * 4, rgba + y * width * 4, rgba + below * width * 4};
        Pixel w[9];
        for (unsigned row = 0; row < 3; ++row) {
            w[3 * row + 1] = sample(rows[row] + (wrap_x ? width - 1 : 0) * 4);
            w[3 * row + 2] = sample(rows[row]);
        }
        for (unsigned x = 0; x < width; ++x) {
            unsigned right = x + 1 < width ? x + 1 : (wrap_x ? 0 : x);
            for (unsigned row = 0; row < 3; ++row) {
                w[3 * row] = w[3 * row + 1];
                w[3 * row + 1] = w[3 * row + 2];
                w[3 * row + 2] = sample(rows[row] + right * 4);
            }
            unsigned mask = 0;
            for (unsigned i = 0; i < 8; ++i)
                mask |= (unsigned)different(w[4], w[i + (i >= 4)]) << i;
            unsigned mirrored = flip_x(mask);
            uint32_t *top = out + (2 * y) * (2 * width) + 2 * x;
            uint32_t *bottom = top + 2 * width;
            top[0] = corner(mask, w[0], w[1], w[3], w[4], w[5], w[7]);
            top[1] = corner(mirrored, w[2], w[1], w[5], w[4], w[3], w[7]);
            bottom[0] = corner(flip_y(mask), w[6], w[7], w[3], w[4], w[5], w[1]);
            bottom[1] = corner(flip_y(mirrored), w[8], w[7], w[5], w[4], w[3], w[1]);
        }
    }
}
