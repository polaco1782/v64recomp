#pragma once
#include <cstddef>
#include <cstdint>
#include <cmath>
#include <array>

namespace GoldenEyePackets {
struct Command {
    uint32_t w0, w1;
};
constexpr size_t maxTriangleWords = 44;

inline size_t triangleWords(uint32_t firstWord) {
    unsigned opcode = firstWord >> 24;
    if (opcode < 0xc8 || opcode > 0xcf)
        return 0;
    return 8 + ((opcode & 4) ? 16 : 0) + ((opcode & 2) ? 16 : 0) + ((opcode & 1) ? 4 : 0);
}

/* GoldenEye sends raw RDP triangles through pairs of RSP half commands.
 * Coefficients are data even when their high byte resembles another opcode. */
inline bool unpackTriangle(const Command *commands, size_t available, uint32_t *words, size_t capacity, size_t& count) {
    count = 0;
    if (!commands || !available || commands[0].w0 >> 24 != 0xb4)
        return false;
    size_t length = triangleWords(commands[0].w1);
    if (!length || length > available || length > capacity || !words)
        return false;
    for (size_t i = 0; i < length; i++) {
        unsigned opcode = commands[i].w0 >> 24;
        unsigned expected = i % 2 ? 0xb2 : 0xb4;
        if (opcode != expected && !(i == length - 1 && opcode == 0xb3))
            return false;
    }
    for (size_t i = 0; i < length; i++)
        words[i] = commands[i].w1;
    count = length;
    return true;
}

struct Mesh {
    std::array<float, 12 * 4> position{}, color{};
    std::array<float, 12 * 2> texcoord{};
    size_t triangles = 0;
};
inline float fixed(uint32_t word) {
    return float(int64_t(word) - (word & 0x80000000u ? INT64_C(0x100000000) : 0)) / 65536.0f;
}
inline float yCoordinate(uint32_t word) {
    int value = int(word & 0x3fff);
    if (value & 0x2000)
        value -= 0x4000;
    return float(value) / 4;
}
inline std::array<float, 4> coefficient(const uint32_t *words, size_t index) {
    std::array<float, 4> out{};
    for (size_t i = 0; i < 4; i++) {
        unsigned shift = i % 2 ? 0 : 16;
        out[i] =
            fixed((((words[index + i / 2] >> shift) & 65535) << 16) | ((words[index + 4 + i / 2] >> shift) & 65535));
    }
    return out;
}

// RDP edges bound two scanline bands. A command can cover a trapezoid with
// nonzero width at YH or YL; reducing it to three vertices loses that area.
inline bool makeMesh(const uint32_t *words, size_t count, bool perspective, Mesh& mesh) {
    mesh = {};
    if (!words || count != triangleWords(words[0]) || !count)
        return false;
    const unsigned opcode = words[0] >> 24;
    const float yh = yCoordinate(words[1]), ym = yCoordinate(words[1] >> 16), yl = yCoordinate(words[0]);
    if (yh > ym || ym > yl)
        return false;
    const float floorYh = std::floor(yh);
    const float xh = fixed(words[4]), dxh = fixed(words[5]);
    const float xm = fixed(words[6]), dxm = fixed(words[7]);
    const float xl = fixed(words[2]), dxl = fixed(words[3]);
    std::array<float, 4> baseColor{}, colorDx{}, colorDe{}, baseTexture{}, textureDx{}, textureDe{};
    size_t offset = 8;
    if (opcode & 4) {
        baseColor = coefficient(words + offset, 0);
        colorDx = coefficient(words + offset, 2);
        colorDe = coefficient(words + offset, 8);
        offset += 16;
    }
    if (opcode & 2) {
        baseTexture = coefficient(words + offset, 0);
        textureDx = coefficient(words + offset, 2);
        textureDe = coefficient(words + offset, 8);
        offset += 16;
    }
    std::array<float, 3> depth{};
    if (opcode & 1)
        for (size_t i = 0; i < 3; i++)
            depth[i] = fixed(words[offset + i]) / 32768.0f;
    auto major = [&](float y) { return xh + dxh * (y - floorYh); };
    auto append = [&](float x, float y, size_t index) {
        float dy = y - floorYh, dx = x - major(y);
        float w = 1, s = 0, t = 0, z = 0;
        if (opcode & 2) {
            s = baseTexture[0] + textureDe[0] * dy + textureDx[0] * dx;
            t = baseTexture[1] + textureDe[1] * dy + textureDx[1] * dx;
            if (perspective) {
                float reciprocal = baseTexture[2] + textureDe[2] * dy + textureDx[2] * dx;
                if (!std::isfinite(reciprocal) || reciprocal == 0)
                    return false;
                w = 65536000.0f / reciprocal;
                s = s / reciprocal * 1024;
                t = t / reciprocal * 1024;
            } else {
                s /= 16;
                t /= 16;
            }
        }
        if (opcode & 1)
            z = depth[0] + depth[2] * dy + depth[1] * dx;
        mesh.position[index * 4] = x;
        mesh.position[index * 4 + 1] = y;
        mesh.position[index * 4 + 2] = z;
        mesh.position[index * 4 + 3] = w;
        mesh.texcoord[index * 2] = s;
        mesh.texcoord[index * 2 + 1] = t;
        for (size_t i = 0; i < 4; i++)
            mesh.color[index * 4 + i] = (baseColor[i] + colorDe[i] * dy + colorDx[i] * dx) / 255;
        return std::isfinite(w) && std::isfinite(s) && std::isfinite(t);
    };
    auto band = [&](float top, float bottom, float minorTop, float minorBottom) {
        if (top == bottom)
            return true;
        std::array<std::array<float, 2>, 4> corners{
            {{major(top), top}, {minorTop, top}, {minorBottom, bottom}, {major(bottom), bottom}}};
        constexpr unsigned indices[2][3] = {{0, 1, 2}, {0, 2, 3}};
        for (const auto& triangle : indices) {
            const auto &a = corners[triangle[0]], &b = corners[triangle[1]], &c = corners[triangle[2]];
            float area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
            if (area == 0)
                continue;
            for (size_t i = 0; i < 3; i++)
                if (!append(corners[triangle[i]][0], corners[triangle[i]][1], mesh.triangles * 3 + i))
                    return false;
            mesh.triangles++;
        }
        return true;
    };
    return band(yh, ym, xm + dxm * (yh - floorYh), xm + dxm * (ym - floorYh)) && band(ym, yl, xl, xl + dxl * (yl - ym));
}
}
