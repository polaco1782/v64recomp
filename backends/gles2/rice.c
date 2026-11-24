// SPDX-License-Identifier: GPL-2.0-only
// RiceVideo / GLideN64 replacement checksums. See licenses/GLideN64.txt.
#include "rice.h"
#include <SDL.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void capture_palette(RicePalette *p, unsigned dest, unsigned count, uint64_t serial, const uint8_t *ram,
                            size_t size, uint64_t source) {
    if (dest < 256 || dest >= 512)
        return;
    count = SDL_min(count, 512 - dest);
    if (source > size || count * 2 > size - source)
        return;
    memcpy(p->bytes + (dest - 256) * 2, ram + source, count * 2);
    for (unsigned i = 0; i < count; ++i)
        p->loads[dest - 256 + i] = serial;
}

void rice_record_load(RiceState *state, uint64_t serial, const Tile *tile, uint32_t w0, uint32_t w1,
                      unsigned image_width, unsigned image_size, const TextureTransfer *dma, const uint8_t *ram,
                      size_t ram_size, uint32_t address) {
    RiceLoad *r = &state->loads[tile->tmem];
    r->serial = 0;
    r->tile = *tile;
    r->tile.sl = w0 >> 12 & 4095;
    r->tile.tl = w0 & 4095;
    r->tile.sh = w1 >> 12 & 4095;
    r->tile.th = w1 & 4095;
    r->opcode = (w0 >> 24) | 0xc0;
    r->image_width = image_width;
    r->image_size = image_size;
    if (r->opcode == 0xf0) {
        // TexFilterPalette starts at SETTIMG. The RDP may use an offset in that image.
        capture_palette(&state->glide, tile->tmem, dma->width * dma->rows, serial, ram, ram_size, address);
        for (unsigned y = 0; y < dma->rows; ++y)
            capture_palette(&state->dma, (tile->tmem + y * tile->line * 4) & 511, dma->width, serial, ram, ram_size,
                            address + (dma->first_bit + (uint64_t)y * dma->stride_bits) / 8);
        return;
    }
    uint64_t row = (dma->row_bits + (dma->first_bit & 7) + 7) / 8, stride = dma->stride_bits / 8;
    uint64_t span = (dma->rows - 1) * stride + row, start = address + dma->first_bit / 8;
    uint64_t origin = address + (((uint64_t)(r->tile.sl >> 2) << image_size) >> 1) +
                      (((uint64_t)image_width << image_size) >> 1) * (r->opcode == 0xf3 ? r->tile.tl : r->tile.tl >> 2);
    if (!row || start != origin || (dma->rows > 1 && (dma->stride_bits & 7)) || span > 65536 || start > ram_size ||
        span > ram_size - start)
        return;
    if (span > r->capacity) {
        uint8_t *bytes = realloc(r->bytes, (size_t)span);
        if (!bytes)
            return;
        r->bytes = bytes;
        r->capacity = (size_t)span;
    }
    if (dma->rows == 1 || stride == row)
        memcpy(r->bytes, ram + start, (size_t)span);
    else
        for (unsigned y = 0; y < dma->rows; ++y)
            memcpy(r->bytes + y * stride, ram + start + y * stride, (size_t)row);
    r->size = (size_t)span;
    r->row_bytes = (unsigned)row;
    r->serial = serial;
}

static unsigned extent(unsigned low, unsigned high) {
    return high / 4 >= low / 4 ? 1 + high / 4 - low / 4 : 1;
}
static unsigned reverse_dxt(unsigned dxt, unsigned width, unsigned size) {
    if (dxt <= 1 || dxt == 2048)
        return 1;
    unsigned low = 2047 / dxt, high = 2047 / (dxt - 1);
    if ((low ? (2048 + low - 1) / low : 1) > dxt)
        ++low;
    unsigned words = SDL_max(1u, (width << size) / 16);
    return words >= low && words <= high ? words : (low + high) / 2;
}
static uint32_t checksum(const uint8_t *bytes, unsigned row, unsigned height, unsigned stride) {
    uint32_t crc = 0;
    for (unsigned y = 0; y < height; ++y) {
        uint32_t word = 0;
        for (int x = (int)row - 4; x >= 0; x -= 4) {
            memcpy(&word, bytes + (size_t)y * stride + x, sizeof(word));
            word ^= (unsigned)x;
            crc = ((crc << 4) | (crc >> 28)) + word;
        }
        crc += word ^ (height - 1 - y);
    }
    return crc;
}
static bool owns(const Gles2RiceWord *word, uint64_t serial, uint16_t mask) {
    return word->load == serial && (word->nibbles & mask) == mask;
}
static bool palette_key(char key[22], const RicePalette *p, const Gles2RiceWord *words, const Tile *tile,
                        unsigned count, uint32_t crc) {
    unsigned first = tile->siz == 0 ? tile->palette * 16 : 0;
    for (unsigned i = first; i < first + count; ++i)
        if (!p->loads[i] || !owns(&words[256 + i], p->loads[i], UINT16_MAX))
            return false;
    snprintf(key, 22, "%08" PRIx32 "#%u#%u#%08" PRIx32, crc, tile->fmt, tile->siz,
             checksum(p->bytes + first * 2, count * 2, 1, 0));
    return true;
}
bool rice_keys(const RiceState *state, const Tile *tile, unsigned tlut, const Gles2RiceWord *words,
               unsigned sampled_width, unsigned sampled_height, char glide[22], char dma[22]) {
    glide[0] = dma[0] = 0;
    const RiceLoad *r = &state->loads[tile->tmem];
    uint64_t serial = words[tile->tmem].load;
    if (!serial || (serial & 511) != tile->tmem || r->serial != serial || !r->image_width)
        return false;
    bool indexed = (tlut & 2) != 0, split = !indexed && tile->siz == 3;
    if (indexed && tile->siz > 1)
        return false;
    unsigned row_nibbles = sampled_width << SDL_min(tile->siz, 2u);
    for (unsigned y = 0; y < sampled_height; ++y)
        for (unsigned n = 0; n < row_nibbles; n += 16) {
            unsigned word = (tile->tmem + y * tile->line + n / 16) & ((indexed || split) ? 255 : 511);
            uint16_t mask = (uint16_t)((1u << SDL_min(16u, row_nibbles - n)) - 1);
            if (y & 1)
                mask = (uint16_t)((mask << 8) | (mask >> 8));
            if (!owns(&words[word], serial, mask) || (split && !owns(&words[word + 256], serial, mask)))
                return false;
        }
    unsigned width, height, stride;
    if (r->opcode == 0xf3) {
        unsigned w = extent(tile->sl, tile->sh), h = extent(tile->tl, tile->th);
        width = tile->ms ? 1u << tile->ms : w;
        height = tile->mt ? 1u << tile->mt : h;
        if ((!tile->ms || (tile->cs & 2)) && w <= 256)
            width = SDL_min(width, w);
        if (((!tile->mt || (tile->ct & 2)) && h <= 256) || height > 256)
            height = SDL_min(height, h);
        stride = tile->siz == 3 ? tile->line * 16
                 : r->tile.th   ? reverse_dxt(r->tile.th, r->image_width, r->image_size) * 8
                                : tile->line * 8;
    } else {
        width = extent(r->tile.sl, r->tile.sh) & 1023;
        height = extent(r->tile.tl, r->tile.th) & 1023;
        if (r->tile.ms)
            width = SDL_min(width, 1u << r->tile.ms);
        if (r->tile.mt)
            height = SDL_min(height, 1u << r->tile.mt);
        width = SDL_min(width, r->image_width);
        if (r->tile.siz > tile->siz)
            width <<= r->tile.siz - tile->siz;
        stride = (r->image_width << r->image_size) / 2;
        if ((width << tile->siz) / 2 > r->row_bytes)
            return false;
    }
    unsigned row = (width << tile->siz) / 2;
    if (!width || !height || (uint64_t)(height - 1) * stride + row > r->size)
        return false;
    uint32_t crc = checksum(r->bytes, row, height, stride);
    if (!indexed) {
        snprintf(glide, 22, "%08" PRIx32 "#%u#%u", crc, tile->fmt, tile->siz);
        return true;
    }
    unsigned highest = 0;
    for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < row; ++x) {
            unsigned v = r->bytes[(size_t)y * stride + x];
            highest = SDL_max(highest, tile->siz == 0 ? SDL_max(v >> 4, v & 15) : v);
        }
    bool a = palette_key(glide, &state->glide, words, tile, highest + 1, crc);
    bool b = palette_key(dma, &state->dma, words, tile, highest + 1, crc);
    return a || b;
}
