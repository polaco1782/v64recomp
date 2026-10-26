// Turok's automap uses L3DEX 1.00, recognized but unimplemented by RT64.
// Reuse the repository's verified orthographic line adapter from Hexen;
// command reference: https://github.com/gonetz/GLideN64/blob/master/src/uCodes/L3DEX.cpp
#include "graphics.h"
#include "game.h"
#include "gbi/rt64_gbi_f3d.h"
#include "gbi/rt64_gbi_f3dex.h"
#include "gbi/rt64_gbi_rdp.h"
#include "hle/rt64_interpreter.h"
#include "hle/rt64_workload_queue.h"
#include <openssl/evp.h>
#include <array>
#include <bitset>
#include <cmath>
#include <vector>

namespace {
RT64::GBI line_gbi;
std::bitset<32> loaded;
size_t memory_size;

void bounds(uint32_t address, size_t size) {
    recomp_game_require(address <= memory_size && size <= memory_size - address, "Turok graphics range outside RDRAM",
                        address);
}
bool fingerprint(RT64::State *s, uint32_t address, size_t size, const char *hex) {
    bounds(address, size);
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; i++)
        bytes[i] = s->RDRAM[(address + i) ^ 3];
    std::array<uint8_t, 32> digest{};
    unsigned length = 0;
    recomp_game_require(EVP_Digest(bytes.data(), size, digest.data(), &length, EVP_sha256(), nullptr) && length == 32,
                        "Turok graphics fingerprint failed", address);
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < digest.size(); i++)
        if (hex[i * 2] != digits[digest[i] >> 4] || hex[i * 2 + 1] != digits[digest[i] & 15])
            return false;
    return true;
}
void unsupported(RT64::State *state, RT64::DisplayList **dl) {
    recomp_warn("Turok unsupported L3DEX command",
                uint32_t(reinterpret_cast<uintptr_t>(*dl) - reinterpret_cast<uintptr_t>(state->RDRAM)));
}
void reset(RT64::State *s) {
    RT64::GBI_F3D::reset(s);
    loaded.reset();
}
void vertex(RT64::State *s, RT64::DisplayList **dl) {
    unsigned count = (*dl)->p0(10, 6), first = (*dl)->p0(17, 7);
    recomp_game_require(first < loaded.size() && count <= loaded.size() - first, "Turok L3DEX vertex range invalid",
                        (*dl)->w0);
    bounds(s->rsp->fromSegmentedMasked((*dl)->w1), count * 16);
    RT64::GBI_F3DEX::vertex(s, dl);
    for (unsigned i = first; i < first + count; i++)
        loaded.set(i);
}
void line(RT64::State *s, RT64::DisplayList **dl) {
    auto *rsp = s->rsp.get();
    const uint32_t word = (*dl)->w1;
    const unsigned a = (word >> 17) & 127, b = (word >> 9) & 127;
    recomp_game_require(a < loaded.size() && b < loaded.size() && loaded[a] && loaded[b],
                        "Turok L3DEX line uses unloaded vertex", word);
    const uint32_t mode = rsp->geometryModeStack[rsp->geometryModeStackSize - 1];
    // The verified automap is unlit and orthographic. Do not silently approximate
    // GPU lighting, fog or perspective clipping if another path starts using it.
    if (mode & (G_LIGHTING | G_FOG | G_TEXTURE_GEN)) {
        recomp_warn("Turok unsupported L3DEX geometry mode",
                    uint32_t(reinterpret_cast<uintptr_t>(*dl) - reinterpret_cast<uintptr_t>(s->RDRAM)));
        return;
    }
    auto& draw = s->ext.workloadQueue->workloads[s->ext.workloadQueue->writeCursor].drawData;
    const unsigned ia = rsp->indices[a], ib = rsp->indices[b];
    recomp_game_require(ia < draw.posScreen.size() && ib < draw.posScreen.size(), "Turok L3DEX vertex index invalid",
                        word);
    const auto p = draw.posScreen[ia], q = draw.posScreen[ib];
    // Turok's guOrtho matrix has homogeneous scale 32, unlike Hexen's 1.
    // posScreen already divides by W. Accept any finite positive constant W,
    // while retaining the rejection of perspective and behind-camera lines.
    const auto &matrix = rsp->modelViewProjMatrix;
    const float w = matrix[3][3];
    if (matrix[0][3] != 0.0f || matrix[1][3] != 0.0f || matrix[2][3] != 0.0f ||
        !std::isfinite(w) || w <= 0.0f || draw.posTransformed[ia][3] != w || draw.posTransformed[ib][3] != w) {
        recomp_warn("Turok unsupported perspective L3DEX line",
                    uint32_t(reinterpret_cast<uintptr_t>(*dl) - reinterpret_cast<uintptr_t>(s->RDRAM)));
        return;
    }
    const float dx = q[0] - p[0], dy = q[1] - p[1];
    const float length = std::hypot(dx, dy);
    const float width = 1.5f + int8_t(word & 255) * 0.5f;
    recomp_game_require(std::isfinite(length) && width > 0, "Turok invalid L3DEX line extent", word);
    if (length == 0)
        return; // A segment with identical endpoints has no area.
    const float ox = -dy * width / (2 * length), oy = dx * width / (2 * length);
    const float xy[4][2] = {
        {p[0] + ox, p[1] + oy}, {p[0] - ox, p[1] - oy}, {q[0] + ox, q[1] + oy}, {q[0] - ox, q[1] - oy}};
    constexpr unsigned corners[6] = {0, 1, 2, 2, 1, 3};
    float positions[24], colors[24], texcoords[12] = {};
    for (unsigned v = 0; v < 6; v++) {
        unsigned corner = corners[v], endpoint = corner < 2 ? ia : ib;
        positions[v * 4] = xy[corner][0];
        positions[v * 4 + 1] = xy[corner][1];
        // RSP screen Z is in units of 1/1024; RDP triangles use 1/32768.
        positions[v * 4 + 2] = draw.posScreen[endpoint][2] / 32.0f;
        positions[v * 4 + 3] = 1.0f;
        // L3DEX uses the first endpoint for flat shading.
        unsigned shade = (mode & 0x200) ? endpoint : ia;
        for (unsigned c = 0; c < 4; c++)
            colors[v * 4 + c] = draw.normColBytes[shade * 4 + c] / 255.0f;
    }
    s->rdp->drawTris(2, positions, texcoords, colors, 0, 0);
}
}

bool turok_bind_microcode(void *opaque, size_t ram_size, uint32_t code, uint32_t data) {
    // L3DEX text/data are in the verified decoded main image at 0x800b4ca0 / 0x800f86b0.
    // Ordinary F3DEX tasks continue through RT64's own microcode database.
    if (code != 0xb4ca0)
        return false;
    auto *interpreter = static_cast<RT64::Interpreter *>(opaque);
    memory_size = ram_size;
    recomp_game_require(fingerprint(interpreter->state, code, 0x1000,
                                    "5fa7e9a119c7933103a353b62c9100d71d2b869e168414cc27a69736279c1f18") &&
                            fingerprint(interpreter->state, data, 0x800,
                                        "d686c116ae4f83d84dc7e514b69aa20fc8ca69f9907e278fc6914231ec4367fb"),
                        "Turok unrecognized L3DEX microcode", code);
    if (line_gbi.ucode == RT64::GBIUCode::Unknown) {
        line_gbi.ucode = RT64::GBIUCode::F3DEX;
        RT64::GBI_RDP::setup(&line_gbi, true);
        RT64::GBI_F3DEX::setup(&line_gbi);
        for (auto& command : line_gbi.map)
            if (!command)
                command = unsupported;
        line_gbi.map[F3D_G_VTX] = vertex;
        line_gbi.map[F3D_G_QUAD] = line; // B5 is LINE3D in L3DEX.
        line_gbi.map[F3D_G_TRI1] = unsupported;
        line_gbi.map[F3DEX_G_TRI2] = unsupported;
        line_gbi.map[F3D_G_CULLDL] = unsupported;
        line_gbi.resetFromTask = reset;
        line_gbi.resetFromLoad = reset;
    }
    interpreter->hleGBI = &line_gbi;
    interpreter->UCode.textAddress = code;
    interpreter->UCode.dataAddress = data;
    interpreter->state->rsp->setGBI(&line_gbi);
    return true;
}
