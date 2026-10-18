// Conker's F3DEXBG extension. Command/lighting reference: GLideN64's
// src/uCodes/F3DEX2CBFD.cpp and src/gSP.cpp (GPL-2.0-or-later).
// RT64 supplies the ordinary F3DEX2/RDP state and rendering operations.
#include "graphics.h"
#include "game.h"
#include "runtime.h"
#include "gbi/rt64_gbi_f3dex2.h"
#include "gbi/rt64_gbi_rdp.h"
#include "hle/rt64_interpreter.h"
#include "hle/rt64_workload_queue.h"
#include <openssl/evp.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <vector>

namespace {
struct Light {
    hlslpp::float3 color{}, direction{}, position{};
    float attenuation = 0;
};
struct ConkerGBI {
    RT64::GBI gbi;
    std::array<Light, 12> lights{};
    std::array<float, 16> coordinates{};
    uint32_t normals = 0, light_count = 0;
    bool advanced = false;
    size_t ram_size = 0;
} cbfd;

void bounds(uint32_t address, size_t bytes) {
    recomp_game_require(address <= cbfd.ram_size && bytes <= cbfd.ram_size - address,
          "Conker graphics buffer outside RDRAM", address);
}
uint8_t byte(RT64::State *s, uint32_t address) { return s->RDRAM[address ^ 3]; }
int16_t half(RT64::State *s, uint32_t address) {
    return int16_t(uint16_t(byte(s, address)) << 8 | byte(s, address + 1));
}
hlslpp::float3 unit(hlslpp::float3 v) {
    float length = hlslpp::dot(v, v);
    return length > 0 ? v / std::sqrt(length) : hlslpp::float3(0);
}
hlslpp::float3 inverse_direction(RT64::RSP *rsp, hlslpp::float3 v) {
    return unit(hlslpp::mul(hlslpp::float4(v, 0),
                           hlslpp::transpose(rsp->modelMatrixStack[rsp->modelMatrixStackSize - 1])).xyz);
}
void unsupported(RT64::State *, RT64::DisplayList **dl) {
    recomp_fail("Conker unsupported graphics command", (*dl)->w0);
}
void reset(RT64::State *state) {
    state->rsp->setClipRatioAll(2);
    cbfd.lights = {};
    cbfd.coordinates = {};
    cbfd.normals = cbfd.light_count = 0;
    cbfd.advanced = false;
}
void move_word(RT64::State *s, RT64::DisplayList **dl) {
    const uint32_t w0 = (*dl)->w0, w1 = (*dl)->w1;
    switch ((w0 >> 16) & 255) {
    case G_MW_NUMLIGHT:
        recomp_game_require(w1 % 48 == 0 && w1 / 48 < cbfd.lights.size(), "Conker invalid light count", w1);
        cbfd.light_count = w1 / 48;
        break;
    case G_MW_CLIP: case G_MW_SEGMENT: case G_MW_FOG: case G_MW_PERSPNORM:
        RT64::GBI_F3DEX2::moveWord(s, dl);
        break;
    case 0x10: {
        // Bit 3 selects an unused coordinate form in this microcode.
        if (w0 & 8) return;
        unsigned index = (w0 >> 1) & 3, part = w0 & 0x30;
        recomp_game_require(index < 3 && part != 0x30, "Conker invalid coordinate modifier", w0);
        for (unsigned i = 0; i < 2; i++) {
            uint16_t value = uint16_t(w1 >> (i ? 0 : 16));
            if (part == 0) cbfd.coordinates[index + i] = int16_t(value);
            else if (part == 0x20) cbfd.coordinates[8 + index + i] = int16_t(value);
            else {
                cbfd.coordinates[4 + index + i] = value / 65536.0f;
                cbfd.coordinates[12 + index + i] = cbfd.coordinates[index + i] + value / 65536.0f;
            }
        }
        break;
    }
    default: unsupported(s, dl);
    }
}
void move_mem(RT64::State *s, RT64::DisplayList **dl) {
    uint32_t w0 = (*dl)->w0, address = s->rsp->fromSegmentedMasked((*dl)->w1);
    switch (w0 & 255) {
    case F3DEX2_G_MV_VIEWPORT:
        bounds(address, 16);
        s->rsp->setViewport((*dl)->w1);
        break;
    case F3DEX2_G_MV_LIGHT: {
        unsigned offset = (w0 >> 5) & 0x3fff, index = offset / 48;
        recomp_game_require(offset % 48 == 0, "Conker invalid light offset", w0);
        if (index < 2) {
            bounds(address, 16);
            s->rsp->setLookAt(index, (*dl)->w1);
            break;
        }
        index -= 2;
        recomp_game_require(index < cbfd.lights.size(), "Conker light index outside array", index);
        bounds(address, 40);
        Light &light = cbfd.lights[index];
        light.color = hlslpp::float3(byte(s, address), byte(s, address + 1), byte(s, address + 2)) / 255.0f;
        light.direction = unit(hlslpp::float3(int8_t(byte(s, address + 8)), int8_t(byte(s, address + 9)),
                                             int8_t(byte(s, address + 10))));
        light.position = hlslpp::float3(half(s, address + 32), half(s, address + 34), half(s, address + 36));
        light.attenuation = byte(s, address + 12) / 16.0f;
        break;
    }
    case 14:
        bounds(address, 2);
        cbfd.normals = address;
        break;
    default: unsupported(s, dl);
    }
}
void lighting_mode(RT64::State *, RT64::DisplayList **) { cbfd.advanced = true; }
void tri4(RT64::State *s, RT64::DisplayList **dl) {
    uint32_t a = (*dl)->w0, b = (*dl)->w1;
    s->rsp->drawIndexedTri((a >> 23) & 31, (a >> 18) & 31, ((a >> 15) & 7) * 4 + (b >> 30));
    s->rsp->drawIndexedTri((a >> 10) & 31, (a >> 5) & 31, a & 31);
    s->rsp->drawIndexedTri((b >> 25) & 31, (b >> 20) & 31, (b >> 15) & 31);
    s->rsp->drawIndexedTri((b >> 10) & 31, (b >> 5) & 31, b & 31);
}
void vertex(RT64::State *s, RT64::DisplayList **dl) {
    auto *rsp = s->rsp.get();
    uint32_t count = ((*dl)->w0 >> 12) & 255, end = ((*dl)->w0 >> 1) & 127;
    recomp_game_require(count && count <= end, "Conker invalid vertex range", (*dl)->w0);
    uint32_t first = end - count, address = rsp->fromSegmentedMasked((*dl)->w1);
    bounds(address, count * sizeof(RT64::RSP::Vertex));
    uint32_t &mode = rsp->geometryModeStack[rsp->geometryModeStackSize - 1];
    if (!(mode & G_LIGHTING)) {
        rsp->setVertex((*dl)->w1, count, first);
        return;
    }
    // Keep source vertices intact. RT64 receives the microcode's final colors,
    // then retains its usual transforms, clipping, fog and vertex tracking.
    std::array<RT64::RSP::Vertex, 128> saved{}, cooked{};
    std::memcpy(saved.data(), s->RDRAM + address, count * sizeof(saved[0]));
    cooked = saved;
    auto mvp = rsp->modelViewProjChanged
                   ? hlslpp::mul(rsp->modelMatrixStack[rsp->modelMatrixStackSize - 1],
                                 rsp->viewProjMatrixStack[rsp->projectionMatrixStackSize - 1])
                   : rsp->modelViewProjMatrix;
    std::vector<std::array<float, 2>> generated_uv;
    if (mode & G_TEXTURE_GEN) generated_uv.resize(count);
    bounds(cbfd.normals + first * 2, count * 2);
    for (uint32_t i = 0; i < count; i++) {
        auto &v = cooked[i];
        uint32_t n = cbfd.normals + (first + i) * 2;
        hlslpp::float3 normal = hlslpp::float3(int8_t(byte(s, n)), int8_t(byte(s, n + 1)), int8_t(v.flag)) / 128.0f;
        auto position = hlslpp::mul(hlslpp::float4(v.x, v.y, v.z, 1), mvp);
        hlslpp::float3 p = (position.xyz + hlslpp::float3(cbfd.coordinates[8], cbfd.coordinates[9], cbfd.coordinates[10])) *
                           hlslpp::float3(cbfd.coordinates[12], cbfd.coordinates[13], cbfd.coordinates[14]);
        hlslpp::float3 color = cbfd.lights[cbfd.light_count].color;
        if (cbfd.advanced && cbfd.light_count) {
            const Light &sun = cbfd.lights[cbfd.light_count - 1];
            float weight = hlslpp::dot(normal, inverse_direction(rsp, sun.direction));
            color += sun.color * std::clamp(weight, 0.0f, 1.0f);
        }
        for (int l = int(cbfd.light_count) - 2; l >= 0; l--) {
            const Light &light = cbfd.lights[l];
            auto delta = p - light.position;
            float distance = hlslpp::dot(delta, delta) / 32768.0f;
            float weight = distance > 0 ? std::min(1.0f, light.attenuation / distance) : (light.attenuation > 0 ? 1 : 0);
            if (cbfd.advanced && (mode & 0x00400000)) {
                float directional = hlslpp::dot(normal, inverse_direction(rsp, light.direction));
                weight *= std::clamp(directional, 0.0f, 1.0f);
            }
            color += light.color * weight;
        }
        if (int16_t(v.flag) >= 0) {
            v.color.r = uint8_t(v.color.r * std::clamp(float(color.x), 0.0f, 1.0f));
            v.color.g = uint8_t(v.color.g * std::clamp(float(color.y), 0.0f, 1.0f));
            v.color.b = uint8_t(v.color.b * std::clamp(float(color.z), 0.0f, 1.0f));
        }
        if (!generated_uv.empty()) {
            auto x = inverse_direction(rsp, rsp->lookAt.x), y = inverse_direction(rsp, rsp->lookAt.y);
            for (unsigned axis = 0; axis < 2; axis++) {
                float dot = std::clamp(float(hlslpp::dot(normal, axis ? y : x)), -1.0f, 1.0f);
                float coordinate = (mode & G_TEXTURE_GEN_LINEAR) ? std::acos(-dot) * (1024.0f / 3.141592654f) : (dot + 1) * 512;
                generated_uv[i][axis] = coordinate * (axis ? rsp->textureState.tc : rsp->textureState.sc) / 65536.0f;
            }
        }
    }
    uint32_t original_mode = mode;
    mode &= ~G_LIGHTING;
    std::memcpy(s->RDRAM + address, cooked.data(), count * sizeof(cooked[0]));
    rsp->setVertex((*dl)->w1, count, first);
    std::memcpy(s->RDRAM + address, saved.data(), count * sizeof(saved[0]));
    mode = original_mode;
    if (!generated_uv.empty()) {
        auto &queue = *s->ext.workloadQueue;
        auto &uv = queue.workloads[queue.writeCursor].drawData.tcFloats;
        recomp_game_require(uv.size() >= count * 2, "Conker missing generated texture coordinates", address);
        size_t base = uv.size() - count * 2;
        for (uint32_t i = 0; i < count; i++) for (unsigned axis = 0; axis < 2; axis++)
            uv[base + i * 2 + axis] = generated_uv[i][axis];
    }
}
bool fingerprint(RT64::State *s, uint32_t address, size_t bytes, const char *hex) {
    bounds(address, bytes);
    std::vector<uint8_t> canonical(bytes);
    for (size_t i = 0; i < bytes; i++) canonical[i] = byte(s, address + uint32_t(i));
    std::array<uint8_t, 32> digest{};
    unsigned length = 0;
    recomp_game_require(EVP_Digest(canonical.data(), bytes, digest.data(), &length, EVP_sha256(), nullptr) && length == 32,
          "Conker graphics fingerprint failed", address);
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < digest.size(); i++)
        if (hex[i * 2] != digits[digest[i] >> 4] || hex[i * 2 + 1] != digits[digest[i] & 15]) return false;
    return true;
}
}

bool conker_bind_microcode(void *opaque, size_t ram_size, uint32_t code, uint32_t data) {
    auto *interpreter = static_cast<RT64::Interpreter *>(opaque);
    cbfd.ram_size = ram_size;
    // Complete text is at cartridge 0x36e90..0x38330. Initialized DMEM is
    // 0x3f410..0x3fb90; the remaining 0x80 bytes hold mutable task parameters.
    recomp_game_require(fingerprint(interpreter->state, code, 0x14a0, "55a64255ef5c60046c3e88bb73891f72ee095e0005b4b748f78f5ff296b35cfa") &&
          fingerprint(interpreter->state, data, 0x780, "eded3bafe0d2ea7f1c3b7d2d0633fb38854b200b98763af8d355781e170b3fe8"),
          "Conker unrecognized graphics microcode", code);
    // The intro reads a CI8 render target as RGBA16 without a TLUT. RT64's
    // GPU reinterpretation shader cannot perform that conversion. Its RAM
    // path synchronizes framebuffer pairs and loads the original packed bytes.
    interpreter->state->ext.emulatorConfig->framebuffer.copyWithGPU = false;
    if (cbfd.gbi.ucode == RT64::GBIUCode::Unknown) {
        cbfd.gbi.ucode = RT64::GBIUCode::F3DEX2;
        RT64::GBI_RDP::setup(&cbfd.gbi, true);
        RT64::GBI_F3DEX2::setup(&cbfd.gbi);
        cbfd.gbi.flags.NoN = true;
        for (auto &command : cbfd.gbi.map) if (!command) command = unsupported;
        cbfd.gbi.map[F3DEX2_G_NOOP] = RT64::GBI_RDP::noOp;
        cbfd.gbi.map[F3DEX2_G_MOVEMEM] = move_mem;
        cbfd.gbi.map[F3DEX2_G_MOVEWORD] = move_word;
        cbfd.gbi.map[F3DEX2_G_LOAD_UCODE] = lighting_mode;
        cbfd.gbi.map[F3DEX2_G_VTX] = vertex;
        for (unsigned i = 0x10; i < 0x20; i++) cbfd.gbi.map[i] = tri4;
        cbfd.gbi.resetFromTask = reset;
    }
    interpreter->hleGBI = &cbfd.gbi;
    interpreter->UCode.textAddress = code;
    interpreter->UCode.dataAddress = data;
    interpreter->state->rsp->setGBI(&cbfd.gbi);
    return true;
}
