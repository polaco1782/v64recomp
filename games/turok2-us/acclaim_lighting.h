#ifndef ACCLAIM_LIGHTING_H
#define ACCLAIM_LIGHTING_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { ACCLAIM_LIGHTS = 8, ACCLAIM_MODE = 0x80 };
typedef struct {
    int16_t position[3], radius;
    uint16_t scale;
    uint8_t color[3];
} AcclaimLight;
typedef struct {
    float direction[3], color[3];
} AcclaimDirectional;
typedef enum {
    ACCLAIM_INVALID,
    ACCLAIM_VIEWPORT,
    ACCLAIM_MATRIX,
    ACCLAIM_LOOKAT,
    ACCLAIM_DIRECTIONAL,
    ACCLAIM_COMPACT
} AcclaimMoveKind;
typedef struct {
    AcclaimMoveKind kind;
    unsigned index;
} AcclaimMove;

AcclaimMove acclaim_move(uint32_t word);
void acclaim_decode(AcclaimLight *light, const uint8_t bytes[16]);
bool acclaim_color(const AcclaimLight lights[ACCLAIM_LIGHTS], const float position[3], const float normal[3],
                   const float matrix[9], const AcclaimDirectional *directional, unsigned count, bool lighting,
                   const float base[3], float color[3]);

#ifdef __cplusplus
}
#endif
#endif
