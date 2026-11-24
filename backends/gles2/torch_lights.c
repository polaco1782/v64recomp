#include "internal.h"
#include <math.h>

void recomp_rdp_gles2_set_torch_source(RecompRDPGles2 *s, RecompTorchSource source) {
    s->torch_source = source;
}

static float matrix_element(Runtime *rt, uint32_t address, unsigned index) {
    return (int16_t)recomp_load16(rt, address + 2 * index) + recomp_load16(rt, address + 32 + 2 * index) / 65536.0f;
}

bool recomp_torch_camera(Runtime *rt, uint32_t projection, uint32_t viewing, uint32_t viewport,
                         RecompTorchCamera *camera, RecompTorchLights *lights) {
    float w = -matrix_element(rt, projection, 11), x = matrix_element(rt, projection, 0),
          y = matrix_element(rt, projection, 5), z = matrix_element(rt, projection, 10),
          offset = matrix_element(rt, projection, 14);
    if (w <= 0 || x <= 0 || y <= 0 || offset >= 0 || matrix_element(rt, projection, 15) != 0)
        return false;
    lights->camera[0] = w / x;
    lights->camera[1] = w / y;
    /* At triangle setup the RSP expands viewport Z units by 32. The RDP
     * backend maps the resulting 0..32768 range to window depth. */
    float scale = (int16_t)recomp_load16(rt, viewport + 4) / 1024.0f;
    float translation = (int16_t)recomp_load16(rt, viewport + 12) / 1024.0f;
    if (scale <= 0)
        return false;
    lights->camera[2] = translation - z / w * scale;
    lights->camera[3] = offset / w * scale;
    for (unsigned i = 0; i < 16; ++i)
        camera->view[i] = matrix_element(rt, viewing, i);
    return true;
}
static float distance_squared(const float p[3]) {
    return p[0] * p[0] + p[1] * p[1] + p[2] * p[2];
}
static void view_point(const RecompTorchCamera *camera, float x, float y, float z, float p[4]) {
    for (unsigned i = 0; i < 3; ++i)
        p[i] = x * camera->view[i] + y * camera->view[i + 4] + z * camera->view[i + 8] + camera->view[i + 12];
    p[2] = -p[2];
}
void recomp_torch_player_capsule(RecompTorchLights *lights, const RecompTorchCamera *camera, const float start[3],
                                 const float end[3], float radius) {
    unsigned slot = lights->capsule_count;
    if (slot >= RECOMP_PLAYER_CAPSULE_LIMIT || !isfinite(radius) || radius <= 0)
        return;
    float *a = lights->capsule_start[slot], *b = lights->capsule_end[slot];
    view_point(camera, start[0], start[1], start[2], a);
    view_point(camera, end[0], end[1], end[2], b);
    if (!isfinite(distance_squared(a)) || !isfinite(distance_squared(b)))
        return;
    a[3] = radius;
    lights->capsule_count++;
    /* A light can cast this player's shadow only if its lit sphere reaches
     * the body. Keep the flag with the light for cheap per-pixel rejection. */
    for (unsigned i = 0; i < lights->count; ++i) {
        const float *center = lights->centers[i];
        float mid[3], bone[3];
        for (unsigned j = 0; j < 3; ++j) {
            bone[j] = (b[j] - a[j]) * 0.5f;
            mid[j] = a[j] + bone[j] - center[j];
        }
        float reach = center[3] + radius + sqrtf(distance_squared(bone));
        if (distance_squared(mid) < reach * reach)
            lights->colors[i][3] = 1.0f;
    }
}
void recomp_torch_add(RecompTorchLights *lights, const RecompTorchCamera *camera, float x, float y, float z,
                      float radius, uint8_t red, uint8_t green, uint8_t blue) {
    float p[4];
    view_point(camera, x, y, z, p);
    p[3] = radius;
    if (!isfinite(distance_squared(p)) || radius <= 0 || p[2] < -radius ||
        fabsf(p[0]) > fmaxf(p[2], 0) * lights->camera[0] + radius * (1 + lights->camera[0]) ||
        fabsf(p[1]) > fmaxf(p[2], 0) * lights->camera[1] + radius * (1 + lights->camera[1]))
        return;
    unsigned slot = lights->count;
    if (slot == RECOMP_TORCH_LIMIT) {
        slot = 0;
        for (unsigned i = 1; i < lights->count; i++)
            if (distance_squared(lights->centers[i]) > distance_squared(lights->centers[slot]))
                slot = i;
        if (distance_squared(p) >= distance_squared(lights->centers[slot]))
            return;
    } else
        lights->count++;
    memcpy(lights->centers[slot], p, sizeof(p));
    lights->colors[slot][0] = red / 255.0f;
    lights->colors[slot][1] = green / 255.0f;
    lights->colors[slot][2] = blue / 255.0f;
    lights->colors[slot][3] = 0;
}
