#include "rdp_triangle.h"
#include <math.h>
#include <string.h>

size_t recomp_rdp_triangle_words(uint32_t first) {
    unsigned opcode = first >> 24;
    if (opcode < 0xc8 || opcode > 0xcf)
        return 0;
    return 8 + ((opcode & 4) ? 16 : 0) + ((opcode & 2) ? 16 : 0) + ((opcode & 1) ? 4 : 0);
}
static float fixed(uint32_t word) {
    int64_t value = (int64_t)word - ((word & UINT32_C(0x80000000)) ? INT64_C(0x100000000) : 0);
    return (float)((double)value / 65536);
}
static float coordinate(uint32_t word) {
    int value = (int)(word & 0x3fff);
    return (float)(value - ((value & 0x2000) ? 0x4000 : 0)) / 4;
}
/* RDP dz uses the integer depth gradients, rounded up to a power of two.
 * Its comparison tolerance is eight 18-bit Z units per dz unit. */
static float depth_delta(uint32_t dx, uint32_t dy) {
    unsigned x = dx >> 16, y = dy >> 16;
    x = x & 0x8000 ? (~x & 0x7fff) : x;
    y = y & 0x8000 ? (~y & 0x7fff) : y;
    unsigned sum = x + y, delta = 1;
    while (delta <= sum && delta < 0x8000)
        delta *= 2;
    return (float)delta / 32768;
}
static void coefficients(const uint32_t *words, unsigned index, float out[4]) {
    for (unsigned i = 0; i < 4; i++) {
        unsigned shift = (i & 1) ? 0 : 16;
        uint32_t integer = (words[index + i / 2] >> shift) & 65535;
        uint32_t fraction = (words[index + 4 + i / 2] >> shift) & 65535;
        out[i] = fixed((integer << 16) | fraction);
    }
}
typedef struct TriangleSetup {
    float top, major_x, major_dx;
    float color[4], color_dx[4], color_de[4];
    float texture[4], texture_dx[4], texture_de[4];
    float depth[3];
    bool shade;
} TriangleSetup;

static float major_x(const TriangleSetup *setup, float y) {
    return setup->major_x + setup->major_dx * (y - setup->top);
}
static bool append(RecompRdpTriangle *mesh, const TriangleSetup *setup, float x, float y) {
    if (mesh->count >= RECOMP_RDP_TRIANGLE_VERTICES)
        return false;
    RecompRdpVertex *v = &mesh->vertices[mesh->count];
    float dy = y - setup->top, dx = x - major_x(setup, y);
    v->position[0] = x;
    v->position[1] = y;
    v->position[3] = 1;
    if (mesh->texture) {
        float s = setup->texture[0] + setup->texture_de[0] * dy + setup->texture_dx[0] * dx;
        float t = setup->texture[1] + setup->texture_de[1] * dy + setup->texture_dx[1] * dx;
        v->reciprocal = setup->texture[2] + setup->texture_de[2] * dy + setup->texture_dx[2] * dx;
        v->uv[0] = s / 16;
        v->uv[1] = t / 16;
    }
    if (mesh->depth)
        v->position[2] = setup->depth[0] + setup->depth[2] * dy + setup->depth[1] * dx;
    for (unsigned i = 0; i < 4; i++)
        v->color[i] = setup->shade ? (setup->color[i] + setup->color_de[i] * dy + setup->color_dx[i] * dx) / 255 : 1;
    for (unsigned i = 0; i < 4; i++)
        if (!isfinite(v->position[i]) || !isfinite(v->color[i]))
            return false;
    if (!isfinite(v->uv[0]) || !isfinite(v->uv[1]))
        return false;
    mesh->count++;
    return true;
}
static bool band(RecompRdpTriangle *mesh, const TriangleSetup *setup, float top, float bottom, float minor_top,
                 float minor_bottom) {
    if (top == bottom)
        return true;
    float corners[4][2] = {
        {major_x(setup, top), top}, {minor_top, top}, {minor_bottom, bottom}, {major_x(setup, bottom), bottom}};
    const unsigned order[2][3] = {{0, 1, 2}, {0, 2, 3}};
    for (unsigned i = 0; i < 2; i++) {
        const float *a = corners[order[i][0]], *b = corners[order[i][1]], *c = corners[order[i][2]];
        if ((b[0] - a[0]) * (c[1] - a[1]) == (b[1] - a[1]) * (c[0] - a[0]))
            continue;
        for (unsigned j = 0; j < 3; j++)
            if (!append(mesh, setup, corners[order[i][j]][0], corners[order[i][j]][1]))
                return false;
    }
    return true;
}
bool recomp_rdp_triangle_decode(const uint32_t *words, size_t count, RecompRdpTriangle *mesh) {
    if (!mesh)
        return false;
    memset(mesh, 0, sizeof(*mesh));
    if (!words || !count || count != recomp_rdp_triangle_words(words[0]))
        return false;
    unsigned opcode = words[0] >> 24;
    float top = coordinate(words[1]), middle = coordinate(words[1] >> 16), bottom = coordinate(words[0]);
    if (top > middle || middle > bottom)
        return false;
    TriangleSetup setup = {
        .top = floorf(top), .major_x = fixed(words[4]), .major_dx = fixed(words[5]), .shade = (opcode & 4) != 0};
    unsigned offset = 8;
    if (setup.shade) {
        coefficients(words + offset, 0, setup.color);
        coefficients(words + offset, 2, setup.color_dx);
        coefficients(words + offset, 8, setup.color_de);
        offset += 16;
    }
    mesh->tile = (words[0] >> 16) & 7;
    mesh->texture = (opcode & 2) != 0;
    mesh->depth = (opcode & 1) != 0;
    if (mesh->texture) {
        coefficients(words + offset, 0, setup.texture);
        coefficients(words + offset, 2, setup.texture_dx);
        coefficients(words + offset, 8, setup.texture_de);
        offset += 16;
    }
    if (mesh->depth) {
        for (unsigned i = 0; i < 3; i++)
            setup.depth[i] = fixed(words[offset + i]) / 32768;
        mesh->depth_delta = depth_delta(words[offset + 1], words[offset + 3]);
    }
    float upper_x = fixed(words[6]), upper_dx = fixed(words[7]);
    float lower_x = fixed(words[2]), lower_dx = fixed(words[3]);
    return band(mesh, &setup, top, middle, upper_x + upper_dx * (top - setup.top),
                upper_x + upper_dx * (middle - setup.top)) &&
           band(mesh, &setup, middle, bottom, lower_x, lower_x + lower_dx * (bottom - middle));
}
