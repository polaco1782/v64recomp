#pragma once
#include <array>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace RecompTextures {
// CPU equivalent of RT64's TextureDecoder.hlsli. TMEM is already in RDP byte
// order; odd rows swap 32-bit words and RGBA32 uses both halves of TMEM.
inline std::vector<uint8_t> decode(const uint8_t *tmem, unsigned width, unsigned height, unsigned fmt, unsigned siz,
                                   unsigned start_words, unsigned line_words, unsigned palette, unsigned tlut) {
    if (!width || !height || width > 4096 || height > 4096 || !line_words || siz > 3 || fmt == 1)
        throw std::invalid_argument("unsupported texture dimensions/format");
    using Pixel = std::array<uint8_t, 4>;
    auto rgba16 = [](unsigned v) -> Pixel {
        auto expand = [](unsigned n) { return uint8_t((n << 3) | (n >> 2)); };
        return {expand((v >> 11) & 31), expand((v >> 6) & 31), expand((v >> 1) & 31), uint8_t((v & 1) ? 255 : 0)};
    };
    auto ia16 = [](unsigned v) -> Pixel {
        uint8_t i = v >> 8;
        return {i, i, i, uint8_t(v)};
    };
    bool rgba32 = fmt == 0 && siz == 3;
    unsigned mask = (rgba32 || tlut) ? 2047 : 4095;
    unsigned stride = line_words * 8, address = start_words * 8;
    std::vector<uint8_t> pixels(size_t(width) * height * 4);
    for (unsigned y = 0; y < height; y++)
        for (unsigned x = 0; x < width; x++) {
            unsigned offset = y * stride + ((x << (rgba32 ? 2 : siz)) >> 1);
            auto load = [&](unsigned off, unsigned high = 0) -> unsigned {
                unsigned row = (off / stride) * stride;
                unsigned relative = (y & 1) ? row + (((off - row) / 4) ^ 1) * 4 + (off & 3) : off;
                return tmem[(((address + relative) & mask) | high) & 4095];
            };
            unsigned v0 = load(offset), v1 = load(offset + 1);
            unsigned nibble = (v0 >> ((x & 1) ? 0 : 4)) & 15;
            Pixel color{0, 0, 0, 255};
            if (tlut) {
                unsigned index = siz == 0 ? (palette << 4) | nibble : v0;
                unsigned p = (2048 + index * 8) & 4095;
                unsigned value = (tmem[p] << 8) | tmem[(p + 1) & 4095];
                if (tlut == (2 << 14))
                    color = rgba16(value);
                else if (tlut == (3 << 14))
                    color = ia16(value);
                else
                    throw std::invalid_argument("unsupported palette format");
            } else if (siz == 0) {
                uint8_t i = nibble * 17;
                if (fmt == 3) {
                    unsigned bits = nibble & 14;
                    i = (bits << 4) | (bits << 1) | (bits >> 2);
                    color = {i, i, i, uint8_t((nibble & 1) ? 255 : 0)};
                } else {
                    if (fmt == 2)
                        i = (palette << 4) | nibble;
                    color = {i, i, i, i};
                }
            } else if (siz == 1) {
                uint8_t i = v0;
                color = {i, i, i, i};
                if (fmt == 3) {
                    i = (v0 >> 4) * 17;
                    color = {i, i, i, uint8_t((v0 & 15) * 17)};
                }
            } else if (siz == 2) {
                unsigned value = (v0 << 8) | v1;
                if (fmt == 0)
                    color = rgba16(value);
                else if (fmt == 3)
                    color = ia16(value);
                else
                    color = {uint8_t(v0), uint8_t(v1), uint8_t(v0), uint8_t(v1)};
            } else {
                unsigned off2 = rgba32 ? offset : offset + 2, high = rgba32 ? 2048 : 0;
                unsigned v2 = load(off2, high), v3 = load(off2 + 1, high);
                if (rgba32)
                    color = {uint8_t(v0), uint8_t(v1), uint8_t(v2), uint8_t(v3)};
                // Matches RT64's observed behavior for non-RGBA 32-bit formats.
                else if (x & 1)
                    color = {uint8_t(v0), uint8_t(v1), uint8_t(v0), uint8_t(v1)};
                else
                    color = {uint8_t(v2), uint8_t(v3), uint8_t(v2), uint8_t(v3)};
            }
            for (unsigned c = 0; c < 4; c++)
                pixels[(size_t(y) * width + x) * 4 + c] = color[c];
        }
    return pixels;
}
}
