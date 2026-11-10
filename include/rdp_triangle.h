#ifndef RECOMP_RDP_TRIANGLE_H
#define RECOMP_RDP_TRIANGLE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { RECOMP_RDP_TRIANGLE_WORDS = 44, RECOMP_RDP_TRIANGLE_VERTICES = 12 };
typedef struct RecompRdpVertex {
    float position[4], color[4], uv[2], reciprocal;
} RecompRdpVertex;
typedef struct RecompRdpTriangle {
    RecompRdpVertex vertices[RECOMP_RDP_TRIANGLE_VERTICES];
    unsigned count, tile;
    bool texture, depth;
    float depth_delta;
} RecompRdpTriangle;

/* RDP edge/coefficient packets, in host uint32_t words. The two scanline bands
 * can form trapezoids: decoding only their three named edges loses coverage.
 * Positions use native screen X/Y, normalized depth, affine S/T and reciprocal W. */
size_t recomp_rdp_triangle_words(uint32_t first);
bool recomp_rdp_triangle_decode(const uint32_t *, size_t count, RecompRdpTriangle *);
#endif
