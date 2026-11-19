#include "lighting.h"
#include <math.h>

Turok2AcclaimMove turok2_acclaim_move(uint32_t word) {
    unsigned type = word & 255, offset = ((word >> 8) & 255) * 8;
    if (type == 8)
        return (Turok2AcclaimMove){TUROK2_ACCLAIM_VIEWPORT, 0};
    if (type == 14)
        return (Turok2AcclaimMove){TUROK2_ACCLAIM_MATRIX, 0};
    if (type == 10) {
        if (offset < 96 && offset % 24 == 0)
            return (Turok2AcclaimMove){offset < 48 ? TUROK2_ACCLAIM_LOOKAT : TUROK2_ACCLAIM_DIRECTIONAL,
                                       offset / 24 - (offset < 48 ? 0 : 2)};
        if (offset >= 96 && (offset - 96) % 16 == 0 && (offset - 96) / 16 < TUROK2_ACCLAIM_LIGHTS)
            return (Turok2AcclaimMove){TUROK2_ACCLAIM_COMPACT, (offset - 96) / 16};
    }
    return (Turok2AcclaimMove){TUROK2_ACCLAIM_INVALID, 0};
}

static int16_t signed_half(const uint8_t *bytes) {
    uint16_t value = (uint16_t)((unsigned)bytes[0] << 8 | bytes[1]);
    return (int16_t)(value < 0x8000 ? (int)value : (int)value - 0x10000);
}

void turok2_acclaim_decode(Turok2AcclaimLight *light, const uint8_t bytes[16]) {
    for (unsigned i = 0; i < 3; i++) {
        light->position[i] = signed_half(bytes + 2 * i);
        light->color[i] = bytes[6 + i];
    }
    light->radius = signed_half(bytes + 10);
    light->scale = (uint16_t)((unsigned)bytes[12] << 8 | bytes[13]);
}

static int saturate_half(int value) {
    return value < -32768 ? -32768 : value > 32767 ? 32767 : value;
}

static int coordinate(float value) {
    return value <= -32768.0f ? -32768 : value >= 32767.0f ? 32767 : (int)floorf(value);
}

bool turok2_acclaim_color(const Turok2AcclaimLight lights[TUROK2_ACCLAIM_LIGHTS], const float position[3],
                          const float normal[3], const float matrix[9], const Turok2AcclaimDirectional *directional,
                          unsigned count, bool lighting, const float base[3], float color[3]) {
    for (unsigned axis = 0; axis < 3; axis++)
        if (!isfinite(position[axis]) || !isfinite(normal[axis]) || !isfinite(base[axis]))
            return false;
    for (unsigned i = 0; i < 9; i++)
        if (!isfinite(matrix[i]))
            return false;
    for (unsigned c = 0; c < 3; c++)
        color[c] = base[c];
    if (lighting) {
        for (unsigned l = 0; l < count; l++) {
            float direction[3], length = 0, intensity = 0;
            for (unsigned axis = 0; axis < 3; axis++) {
                direction[axis] = 0;
                for (unsigned k = 0; k < 3; k++)
                    direction[axis] += matrix[axis * 3 + k] * directional[l].direction[k];
                length += direction[axis] * direction[axis];
            }
            if (length > 0) {
                length = sqrtf(length);
                for (unsigned axis = 0; axis < 3; axis++)
                    intensity += normal[axis] * direction[axis] / length;
            }
            if (intensity > 0)
                for (unsigned c = 0; c < 3; c++)
                    color[c] += intensity * directional[l].color[c];
        }
    }
    int vertex[3];
    for (unsigned axis = 0; axis < 3; axis++)
        vertex[axis] = coordinate(position[axis]);
    for (unsigned l = 0; l < TUROK2_ACCLAIM_LIGHTS; l++) {
        if (lights[l].radius < 0)
            continue;
        int distance = 0;
        for (unsigned axis = 0; axis < 3; axis++) {
            int delta = saturate_half((int)lights[l].position[axis] - vertex[axis]);
            if (delta < 0)
                delta = delta == -32768 ? 32767 : -delta;
            distance = saturate_half(distance + delta);
        }
        distance = saturate_half(distance - lights[l].radius);
        if (distance >= 0)
            continue;
        /* VMUDN keeps the low product word; VMUDM then takes the high
         * signed-color/unsigned-weight product. Colors are stored as u8 << 7. */
        uint16_t weight = (uint16_t)((unsigned)-distance * lights[l].scale);
        for (unsigned c = 0; c < 3; c++) {
            unsigned contribution = ((unsigned)lights[l].color[c] * 128 * weight) >> 16;
            color[c] = fminf(color[c] + contribution / 32640.0f, 1.0f);
        }
    }
    for (unsigned c = 0; c < 3; c++)
        color[c] = fminf(fmaxf(color[c], 0.0f), 1.0f);
    return true;
}
