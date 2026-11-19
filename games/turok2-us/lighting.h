#ifndef TUROK2_LIGHTING_H
#define TUROK2_LIGHTING_H
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { TUROK2_ACCLAIM_LIGHTS = 8, TUROK2_ACCLAIM_MODE = 0x80 };
typedef struct {
    int16_t position[3], radius;
    uint16_t scale;
    uint8_t color[3];
} Turok2AcclaimLight;
typedef struct {
    float direction[3], color[3];
} Turok2AcclaimDirectional;
typedef enum {
    TUROK2_ACCLAIM_INVALID,
    TUROK2_ACCLAIM_VIEWPORT,
    TUROK2_ACCLAIM_MATRIX,
    TUROK2_ACCLAIM_LOOKAT,
    TUROK2_ACCLAIM_DIRECTIONAL,
    TUROK2_ACCLAIM_COMPACT
} Turok2AcclaimMoveKind;
typedef struct {
    Turok2AcclaimMoveKind kind;
    unsigned index;
} Turok2AcclaimMove;

Turok2AcclaimMove turok2_acclaim_move(uint32_t word);
void turok2_acclaim_decode(Turok2AcclaimLight *light, const uint8_t bytes[16]);
bool turok2_acclaim_color(const Turok2AcclaimLight lights[TUROK2_ACCLAIM_LIGHTS], const float position[3],
                          const float normal[3], const float matrix[9], const Turok2AcclaimDirectional *directional,
                          unsigned count, bool lighting, const float base[3], float color[3]);

#ifdef __cplusplus
}
#endif
#endif
