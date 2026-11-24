#pragma once
#include "renderer.h"

#ifdef __cplusplus
extern "C" {
#endif
enum { RECOMP_TORCH_LIMIT = 8, RECOMP_PLAYER_CAPSULE_LIMIT = 11 };
typedef struct RecompTorchLights {
    float camera[4];
    float centers[RECOMP_TORCH_LIMIT][4], colors[RECOMP_TORCH_LIMIT][4];
    unsigned count;
    unsigned capsule_count;
    float player_min_depth;
    float capsule_start[RECOMP_PLAYER_CAPSULE_LIMIT][4], capsule_end[RECOMP_PLAYER_CAPSULE_LIMIT][4];
} RecompTorchLights;
typedef struct RecompTorchCamera {
    float view[16];
} RecompTorchCamera;

typedef void (*RecompTorchSource)(Runtime *, RecompTorchLights *);
void recomp_renderer_set_torch_source(RecompRenderer *, RecompTorchSource);
bool recomp_torch_camera(Runtime *, uint32_t projection, uint32_t viewing, uint32_t viewport, RecompTorchCamera *,
                         RecompTorchLights *);
void recomp_torch_player_capsule(RecompTorchLights *, const RecompTorchCamera *, const float start[3],
                                 const float end[3], float radius);
void recomp_torch_add(RecompTorchLights *, const RecompTorchCamera *, float x, float y, float z, float radius,
                      uint8_t red, uint8_t green, uint8_t blue);
#ifdef __cplusplus
}
#endif
