#ifndef RECOMP_GLES2_HQ2X_H
#define RECOMP_GLES2_HQ2X_H
#include <stdbool.h>
#include <stdint.h>

// Scale a nonempty decoded RGBA8 tile into 2*width by 2*height RGBA8 texels.
void gles2_hq2x(const uint8_t *rgba, unsigned width, unsigned height, bool wrap_x, bool wrap_y, uint32_t *out);

#endif
