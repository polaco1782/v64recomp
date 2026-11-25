// Quake US turns its BSP faces into compact world-space windings at load time.
// Walk submodel zero's BSP front to back, independently of the camera PVS.
#include "reflections.h"
#include "controls.h"
#include "quake_controls_config.h"
#include "torch_lights.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#ifdef RECOMP_DEBUG
#include <stdio.h>
#endif

typedef struct {
    float x, y, z;
} Vec;
static Vec add(Vec a, Vec b) {
    return (Vec){a.x + b.x, a.y + b.y, a.z + b.z};
}
static Vec scale(Vec a, float k) {
    return (Vec){a.x * k, a.y * k, a.z * k};
}
static float dot(Vec a, Vec b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static Vec cross(Vec a, Vec b) {
    return (Vec){a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
static bool span(Runtime *rt, uint32_t p, size_t bytes, unsigned align) {
    return p >= 0x80000000u && (p - 0x80000000u) <= rt->rdram_size && !(p & (align - 1)) &&
           bytes <= rt->rdram_size - (p - 0x80000000u);
}
static float real(Runtime *rt, uint32_t p) {
    uint32_t bits = recomp_load32(rt, p);
    float v;
    memcpy(&v, &bits, 4);
    return v;
}
static Vec vector(Runtime *rt, uint32_t p) {
    return (Vec){real(rt, p), real(rt, p + 4), real(rt, p + 8)};
}
typedef struct {
    Vec normal;
    float distance;
    int child[2];
    unsigned first, count;
    bool loaded;
} Node;
typedef struct {
    uint32_t winding, texture, light;
    unsigned points, axis, width, height, shift, stride, light_w, light_h;
    float lo[2], hi[2], offset[2];
    int light_origin[2];
    Vec tex_axis[2];
    bool back;
} Face;
typedef struct {
    uint32_t model, nodes, faces, node_count, face_count, first, count, name[4];
} MapKey;
static struct {
    MapKey key;
    Node *nodes;
    Face *faces;
    float pose[20];
    bool valid, have_pose;
#ifdef RECOMP_DEBUG
    bool reported_offscreen;
#endif
    RecompLevelReflection *image;
} world;

void quake_free_reflections(void) {
    free(world.nodes);
    free(world.faces);
    free(world.image);
    memset(&world, 0, sizeof(world));
}

// Drop the plane normal's largest component for a stable convex polygon test.
static void axes(unsigned axis, unsigned *x, unsigned *y) {
    *x = (axis + 1) % 3;
    *y = (axis + 2) % 3;
}
static int16_t coord(Runtime *rt, uint32_t point, unsigned axis) {
    // read_face checked the entire packed winding when the level was loaded.
    return (int16_t)recomp_ram_load16(rt, (point & 0x1fffffff) + axis * 2);
}
static bool read_face(Runtime *rt, uint32_t p, Face *f) {
    uint32_t poly = recomp_load32(rt, p + 0x24), plane = recomp_load32(rt, p + 4), info = recomp_load32(rt, p + 0x28);
    if (!span(rt, poly, 2, 2) || !span(rt, plane, 16, 4) || !span(rt, info, 40, 4))
        return false;
    f->points = recomp_load16(rt, poly);
    f->winding = poly + 2;
    if (f->points < 3 || f->points > 1024 || !span(rt, f->winding, f->points * 10, 2))
        return false;
    f->back = (recomp_load32(rt, p + 8) & 2) != 0;
    Vec n = vector(rt, plane);
    float abs_normal[3] = {fabsf(n.x), fabsf(n.y), fabsf(n.z)};
    f->axis = abs_normal[1] > abs_normal[0] ? 1 : 0;
    if (abs_normal[2] > abs_normal[f->axis])
        f->axis = 2;
    unsigned ax, ay;
    axes(f->axis, &ax, &ay);
    f->lo[0] = f->lo[1] = 32768;
    f->hi[0] = f->hi[1] = -32769;
    for (unsigned i = 0; i < f->points; i++) {
        uint32_t v = f->winding + i * 10;
        float x = coord(rt, v, ax), y = coord(rt, v, ay);
        f->lo[0] = fminf(x, f->lo[0]);
        f->hi[0] = fmaxf(x, f->hi[0]);
        f->lo[1] = fminf(y, f->lo[1]);
        f->hi[1] = fmaxf(y, f->hi[1]);
    }
    for (unsigned i = 0; i < 2; i++) {
        f->tex_axis[i] = vector(rt, info + i * 16);
        f->offset[i] = real(rt, info + i * 16 + 12);
        if (!isfinite(dot(f->tex_axis[i], f->tex_axis[i])) || !isfinite(f->offset[i]))
            return false;
        f->light_origin[i] = (int16_t)recomp_load16(rt, p + 0x14 + i * 2);
    }
    uint32_t tex = recomp_load32(rt, info + 32);
    if (!span(rt, tex, 60, 4))
        return false;
    f->width = recomp_load32(rt, tex + 16);
    f->height = recomp_load32(rt, tex + 20);
    f->shift = recomp_load32(rt, tex + 32);
    f->texture = recomp_load32(rt, tex + 56);
    if (!f->width || !f->height || f->width > 1024 || f->height > 1024 || f->shift > 8 ||
        !span(rt, f->texture, 2 * f->width * f->height, 2))
        return false;
    f->light_w = (recomp_load16(rt, p + 0x18) >> 4) + 1;
    f->light_h = (recomp_load16(rt, p + 0x1a) >> 4) + 1;
    f->stride = (2 * f->light_w + 7) & ~7u;
    uint32_t light = recomp_load32(rt, p + 0x2c);
    // The loader installs a single shared white sample for unlit faces.
    if (!(recomp_load32(rt, p + 8) & 0x14) && light != 0x80078250 && span(rt, light, f->stride * f->light_h, 2))
        f->light = light;
    return true;
}
// Only submodel zero belongs to the static scene. Other roots in the same
// arrays describe doors, lifts and other brush entities with separate transforms.
static bool load_node(Runtime *rt, const MapKey *key, unsigned index, unsigned depth) {
    Node *node = &world.nodes[index];
    if (depth > 128 || node->loaded)
        return false;
    node->loaded = true;
    uint32_t p = key->nodes + index * 52;
    uint32_t plane = recomp_load32(rt, p + 36);
    if (!span(rt, plane, 16, 4))
        return false;
    node->normal = vector(rt, plane);
    node->distance = real(rt, plane + 12);
    if (!isfinite(node->distance) || !(fabsf(dot(node->normal, node->normal) - 1) <= 0.01f))
        return false;
    unsigned first = recomp_load16(rt, p + 48);
    node->count = recomp_load16(rt, p + 50);
    if (node->count) {
        if (first < key->first || first - key->first >= key->count || node->count > key->count - (first - key->first))
            return false;
        node->first = first - key->first;
    }
    for (unsigned side = 0; side < 2; side++) {
        uint32_t child = recomp_load32(rt, p + 40 + side * 4);
        if (!span(rt, child, 4, 4))
            return false;
        if ((int32_t)recomp_load32(rt, child) < 0) {
            node->child[side] = -1;
            continue;
        }
        uint32_t offset = child - key->nodes;
        if (child < key->nodes || offset >= key->node_count * 52 || offset % 52)
            return false;
        node->child[side] = (int)(offset / 52);
        if (!load_node(rt, key, (unsigned)node->child[side], depth + 1))
            return false;
    }
    return true;
}
static bool load_world(Runtime *rt, MapKey k) {
    if (!k.count || k.first > k.face_count || k.count > k.face_count - k.first || k.face_count > 65535 ||
        !k.node_count || k.node_count > 65535 || !span(rt, k.nodes, k.node_count * 52, 4) ||
        !span(rt, k.faces, k.face_count * 48, 4))
        return false;
    world.faces = calloc(k.count, sizeof(Face));
    world.nodes = calloc(k.node_count, sizeof(Node));
    if (!world.faces || !world.nodes)
        return false;
    for (unsigned i = 0; i < k.count; i++)
        if (!read_face(rt, k.faces + (k.first + i) * 48, &world.faces[i]))
            return false;
    return load_node(rt, &k, 0, 0);
}
static bool contains(Runtime *rt, const Face *f, Vec p) {
    float coord3[] = {p.x, p.y, p.z};
    unsigned ax, ay;
    axes(f->axis, &ax, &ay);
    float x = coord3[ax], y = coord3[ay];
    if (x < f->lo[0] || x > f->hi[0] || y < f->lo[1] || y > f->hi[1])
        return false;
    uint32_t last = f->winding + 10 * (f->points - 1);
    float lx = coord(rt, last, ax), ly = coord(rt, last, ay);
    bool neg = false, pos = false;
    for (unsigned i = 0; i < f->points; i++) {
        uint32_t v = f->winding + 10 * i;
        float vx = coord(rt, v, ax), vy = coord(rt, v, ay), side = (vx - lx) * (y - ly) - (vy - ly) * (x - lx);
        neg |= side < -0.01f;
        pos |= side > 0.01f;
        if (neg && pos)
            return false;
        lx = vx;
        ly = vy;
    }
    return true;
}
typedef struct {
    const Face *face;
    Vec point, normal;
    float t;
} Hit;
static bool trace(Runtime *rt, int index, Vec origin, Vec dir, float lo, float hi, Hit *hit) {
    // Split the segment at this plane. A hit in the near subtree precedes every
    // coplanar face and every hit in the far subtree. Negative indices are leaves.
    if (index < 0)
        return false;
    const Node *n = &world.nodes[index];
    float start = dot(origin, n->normal) - n->distance, velocity = dot(dir, n->normal);
    bool side = (start + lo * velocity) < 0;
    if (side == ((start + hi * velocity) < 0) || velocity == 0)
        return trace(rt, n->child[side], origin, dir, lo, hi, hit);
    float t = -start / velocity;
    if (trace(rt, n->child[side], origin, dir, lo, t, hit))
        return true;
    Vec point = add(origin, scale(dir, t));
    for (unsigned i = 0; i < n->count; i++) {
        const Face *face = &world.faces[n->first + i];
        if (face->back == side && contains(rt, face, point)) {
            *hit = (Hit){face, point, scale(n->normal, side ? -1 : 1), t};
            return true;
        }
    }
    return trace(rt, n->child[!side], origin, dir, t, hi, hit);
}
static unsigned repeat_texel(float coord, unsigned size) {
    int index = (int)floorf(fmodf(coord, (float)size));
    return (unsigned)(index < 0 ? index + (int)size : index);
}
static void radiance(Runtime *rt, const Hit *hit, float rgb[3]) {
    const Face *f = hit->face;
    float u = dot(f->tex_axis[0], hit->point) + f->offset[0], v = dot(f->tex_axis[1], hit->point) + f->offset[1];
    float k = 1.0f / (1u << f->shift);
    unsigned x = repeat_texel(u * k, f->width), y = repeat_texel(v * k, f->height);
    uint16_t tex = recomp_load16(rt, f->texture + 2 * (y * f->width + x)), light = 0xffff;
    if (f->light) {
        x = (unsigned)fminf(f->light_w - 1, fmaxf(0, (u - f->light_origin[0]) / 16 + 0.5f));
        y = (unsigned)fminf(f->light_h - 1, fmaxf(0, (v - f->light_origin[1]) / 16 + 0.5f));
        light = recomp_load16(rt, f->light + y * f->stride + 2 * x);
    }
    for (unsigned c = 0; c < 3; c++) {
        unsigned shift = 11 - 5 * c;
        rgb[c] = ((tex >> shift) & 31) * ((light >> shift) & 31) / (31.0f * 31.0f);
    }
}
const RecompLevelReflection *quake_level_reflections(Runtime *rt) {
    if (!quake_single_player_view(rt)) {
        world.key.model = 0;
        world.have_pose = false;
        return NULL;
    }
    // The US world renderer (0x80025a08) reads the model here and starts at its root node.
    uint32_t m = recomp_load32(rt, 0x8008da00);
    if (!span(rt, m, 0x168, 4) || recomp_load32(rt, m + 0x14) != 0)
        return NULL;
    MapKey key = {.model = m,
                  .nodes = recomp_load32(rt, m + 0x8c),
                  .node_count = recomp_load32(rt, m + 0x88),
                  .faces = recomp_load32(rt, m + 0x9c),
                  .face_count = recomp_load32(rt, m + 0x98),
                  .first = recomp_load32(rt, m + 0x58),
                  .count = recomp_load32(rt, m + 0x5c)};
    for (unsigned i = 0; i < 4; i++)
        key.name[i] = recomp_load32(rt, m + 4 * i);
    if (memcmp(&key, &world.key, sizeof(key))) {
        free(world.faces);
        free(world.nodes);
        world.faces = NULL;
        world.nodes = NULL;
        world.have_pose = false;
        world.key = key;
        world.valid = load_world(rt, key);
#ifdef RECOMP_DEBUG
        world.reported_offscreen = false;
#endif
    }
    if (!world.valid)
        return NULL;
    RecompTorchCamera camera;
    RecompTorchLights proj;
    if (!recomp_torch_camera(rt, Q_CTL_PROJECTION_MATRIX, 0x800781a0, 0x8006c2d0, &camera, &proj))
        return NULL;
    float pose[20];
    memcpy(pose, camera.view, sizeof(camera.view));
    memcpy(pose + 16, proj.camera, sizeof(proj.camera));
    if (world.have_pose && !memcmp(world.pose, pose, sizeof(pose)))
        return world.image;
    // Invert the linear part of the guest world-to-eye transform, retaining its scale.
    Vec a = {pose[0], pose[4], pose[8]}, b = {pose[1], pose[5], pose[9]}, c = {pose[2], pose[6], pose[10]};
    float det = dot(a, cross(b, c));
    if (!isfinite(det) || fabsf(det) < 0.00001f)
        return NULL;
    Vec right = scale(cross(b, c), 1 / det), up = scale(cross(c, a), 1 / det), back = scale(cross(a, b), 1 / det);
    Vec eye = add(add(scale(right, -pose[12]), scale(up, -pose[13])), scale(back, -pose[14]));
    if (!world.image)
        world.image = calloc(1, sizeof(*world.image));
    if (!world.image)
        return NULL;
#ifdef RECOMP_DEBUG
    bool first = !world.have_pose;
    unsigned hits = 0, offscreen = 0;
#endif
    world.have_pose = true;
    memcpy(world.pose, pose, sizeof(pose));
    ++world.image->serial;
    for (unsigned y = 0; y < RECOMP_REFLECTION_HEIGHT; y++)
        for (unsigned x = 0; x < RECOMP_REFLECTION_WIDTH; x++) {
            float *pixel = world.image->rgba[y][x];
            pixel[3] = 0;
            float dx = (2 * (x + 0.5f) / RECOMP_REFLECTION_WIDTH - 1) * pose[16],
                  dy = (2 * (y + 0.5f) / RECOMP_REFLECTION_HEIGHT - 1) * pose[17];
            Vec dir = add(add(scale(right, dx), scale(up, dy)), scale(back, -1));
            Hit floor, hit;
            if (!trace(rt, 0, eye, dir, 0.01f, 8192, &floor) || floor.normal.z < 0.65f)
                continue;
            Vec ray = add(dir, scale(floor.normal, -2 * dot(dir, floor.normal)));
            if (!trace(rt, 0, add(floor.point, scale(floor.normal, 0.25f)), ray, 0.01f, 8192, &hit))
                continue;
            float window_z = pose[18] + pose[19] / floor.t;
            if (window_z <= 0 || window_z >= 1)
                continue;
            radiance(rt, &hit, pixel);
            pixel[3] = 1 / (1 - window_z);
#ifdef RECOMP_DEBUG
            ++hits;
            Vec delta = add(hit.point, scale(eye, -1));
            float z = -dot(c, delta);
            offscreen += z <= 0 || fabsf(dot(a, delta)) > z * pose[16] || fabsf(dot(b, delta)) > z * pose[17];
#endif
        }
#ifdef RECOMP_DEBUG
    if (first || (offscreen && !world.reported_offscreen))
        fprintf(stderr, "Quake level reflections: %u BSP faces, %u reflected samples (%u off screen)\n", key.count,
                hits, offscreen);
    world.reported_offscreen |= offscreen != 0;
#endif
    return world.image;
}
