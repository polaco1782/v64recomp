#include "internal.h"
#include <math.h>
#include <stdlib.h>

/* The compiled RSP emits projected edges. A packet has no object ID, so
 * pair nearby draws with the same material, projected shape, and motion
 * consistent across the entire packet. Reject large jumps and scene changes. */
typedef struct TrianglePose {
    uint32_t mux0, mux1, texture, mode, other_high, flags;
    float anchor[3][3], center[2], area;
    bool used;
} TrianglePose;

enum { MAX_POSES = 16384, CORNER_BUCKETS = 4096 };
typedef struct CornerSurface {
    float gradient[2], radius;
    uint32_t texture;
    bool depth;
} CornerSurface;
typedef struct Corner {
    float position[3], delta[3];
    CornerSurface surface;
    unsigned next;
} Corner;
typedef struct FramePoses {
    TrianglePose *poses;
    size_t count, capacity;
    unsigned width, height;
} FramePoses;
typedef struct Interpolation {
    FramePoses current, previous;
    size_t matched;
    Corner *corners;
    unsigned buckets[CORNER_BUCKETS], corner_count, corner_capacity;
    bool moved, ineligible, in_frame;
    Surface *target;
    GLuint depth;
    uint64_t serial, displayed;
    unsigned ticks;
    Vertex batch[RECOMP_GLES2_BATCH_VERTICES];
} Interpolation;

void recomp_gles2_init_interpolation(RecompRDPGles2 *s) {
    s->interpolation = calloc(1, sizeof(Interpolation));
    if (!s->interpolation)
        recomp_fail("GLES interpolation allocation failed", 0);
}
void recomp_gles2_destroy_interpolation(RecompRDPGles2 *s) {
    Interpolation *i = s->interpolation;
    if (i) {
        for (unsigned n = 0; n < 8; n++) {
            if (s->midpoints[n].depth)
                s->gl.DeleteRenderbuffers(1, &s->midpoints[n].depth);
            recomp_gles2_destroy_surface(s, &s->midpoints[n]);
        }
        free(i->corners);
        free(i->previous.poses);
        free(i->current.poses);
        free(i);
        s->interpolation = NULL;
    }
}
void recomp_gles2_begin_interpolation(RecompRDPGles2 *s) {
    Interpolation *i = s->interpolation;
    /* Games may split one frame across several RSP submissions before FULLSYNC. */
    if (i->in_frame)
        return;
    i->in_frame = true;
    FramePoses spare = i->previous;
    i->previous = i->current;
    i->current = spare;
    i->current.count = 0;
    i->matched = 0;
    i->corner_count = 0;
    memset(i->buckets, 0, sizeof(i->buckets));
    i->moved = i->ineligible = false;
    i->target = NULL;
    i->serial++;
}

/* Seed from the native attachments immediately before the frame's first color
 * write. Partial redraws retain the same background and initial depth. Only
 * the native FBO is ever used by guest memory readback. */
Surface *recomp_gles2_midpoint_target(RecompRDPGles2 *s) {
    Interpolation *i = s->interpolation;
    Surface *f = s->surface;
    f->serial = i->serial;
    f->has_midpoint = false;
    if (s->color_siz < 2 || s->wireframe)
        i->ineligible = true;
    if (i->target && i->target != f)
        i->ineligible = true;
    if (i->ineligible)
        return NULL;
    Surface *mid = &s->midpoints[f - s->surfaces];
    if (i->target == f) {
        if (i->depth != f->depth || i->current.width != f->width || i->current.height != f->height) {
            i->ineligible = true;
            return NULL;
        }
        return mid->serial == i->serial ? mid : NULL;
    }
    i->target = f;
    i->depth = f->depth;
    i->current.width = f->width;
    i->current.height = f->height;
    if (!i->previous.count || i->previous.width != f->width || i->previous.height != f->height)
        return NULL;
    if (mid->valid && (mid->width != f->width || mid->height != f->height)) {
        s->gl.DeleteRenderbuffers(1, &mid->depth);
        recomp_gles2_destroy_surface(s, mid);
    }
    if (!mid->valid) {
        recomp_gles2_create_surface(s, mid, f->width, f->height);
        s->gl.GenRenderbuffers(1, &mid->depth);
        s->gl.BindRenderbuffer(GL_RENDERBUFFER, mid->depth);
        s->gl.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24_OES, f->width, f->height);
        s->gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, mid->depth);
        if (s->gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            recomp_fail("GLES interpolation depth attachment incomplete", f->address);
    }
    s->gl.BindFramebuffer(GL_FRAMEBUFFER, mid->framebuffer);
    if (f->scene_depth)
        recomp_gles2_bind_scene_depth(s, mid);
    GLenum targets[3] = {GL_COLOR_ATTACHMENT0, GL_NONE, GL_NONE};
    unsigned target_count = 1;
    if (f->scene_depth) {
        targets[1] = GL_COLOR_ATTACHMENT1_EXT;
        target_count = 2;
    }
#ifdef RECOMP_GLES2_TEXTURE_PACKS
    if (f->pick_texture) {
        recomp_gles2_pick_attach(s, mid);
        targets[2] = GL_COLOR_ATTACHMENT2_EXT;
        target_count = 3;
    }
#endif
    s->gl.BindFramebuffer(GL_READ_FRAMEBUFFER_ANGLE, f->framebuffer);
    s->gl.Disable(GL_SCISSOR_TEST);
    for (unsigned k = 0; k < target_count; ++k) {
        if (targets[k] == GL_NONE)
            continue;
        GLenum only[3] = {GL_NONE, GL_NONE, GL_NONE};
        only[k] = targets[k];
        s->gl.DrawBuffers((GLsizei)target_count, only);
        s->gl.ReadBuffer(targets[k]);
        s->gl.BlitFramebuffer(0, 0, f->width, f->height, 0, 0, mid->width, mid->height,
                              GL_COLOR_BUFFER_BIT | (k == 0 ? GL_DEPTH_BUFFER_BIT : 0), GL_NEAREST);
    }
    s->gl.ReadBuffer(GL_COLOR_ATTACHMENT0);
    s->gl.DrawBuffers((GLsizei)target_count, targets);
    s->gl.BindFramebuffer(GL_FRAMEBUFFER, f->framebuffer);
    mid->serial = i->serial;
    return mid;
}

/* RDP edge setup can split the same triangle into different numbers of GLES
 * triangles on successive frames. Use the ends of its major edge and the
 * farthest point on its minor edge as a basis for both scanline bands. */
static float cross(float ax, float ay, float bx, float by) {
    return ax * by - ay * bx;
}
static void triangle_basis(TrianglePose *pose, const Vertex *v, unsigned count, bool left_major) {
    const float *top = v[0].position, *bottom = top, *tip = top;
    for (unsigned k = 1; k < count; k++) {
        const float *q = v[k].position;
        if (q[1] > top[1] || (q[1] == top[1] && (left_major ? q[0] < top[0] : q[0] > top[0])))
            top = q;
        if (q[1] < bottom[1] || (q[1] == bottom[1] && (left_major ? q[0] < bottom[0] : q[0] > bottom[0])))
            bottom = q;
    }
    float dx = bottom[0] - top[0], dy = bottom[1] - top[1], area = 0;
    for (unsigned k = 0; k < count; k++) {
        const float *q = v[k].position;
        float a = cross(dx, dy, q[0] - top[0], q[1] - top[1]);
        if (fabsf(a) > fabsf(area)) {
            area = a;
            tip = q;
        }
    }
    const float *points[] = {top, bottom, tip};
    for (unsigned k = 0; k < 3; k++)
        memcpy(pose->anchor[k], points[k], sizeof(pose->anchor[k]));
    for (unsigned k = 0; k < 2; k++)
        pose->center[k] = (top[k] + bottom[k] + tip[k]) / 3;
    pose->area = area;
}

/* First three preserve winding. Last three reverse the corner enumeration. */
static const unsigned permutations[6][3] = {{0, 1, 2}, {1, 2, 0}, {2, 0, 1}, {0, 2, 1}, {2, 1, 0}, {1, 0, 2}};

/* Clipping can insert or remove packets before a surviving triangle. Search
 * nearby draws, comparing shape after subtracting translation. Allow corner
 * order to change when two projected vertices cross the same scanline. */
static float pose_error(const TrianglePose *p, const TrianglePose *old, unsigned *order) {
    if (old->used || p->flags != old->flags || p->mode != old->mode || p->other_high != old->other_high ||
        p->texture != old->texture || p->mux0 != old->mux0 || p->mux1 != old->mux1)
        return 1;
    float dx = old->center[0] - p->center[0], dy = old->center[1] - p->center[1];
    if (fabsf(dx) > 0.16f || fabsf(dy) > 0.16f)
        return 1;
    float best = 0.04f;
    unsigned first = (p->area < 0) == (old->area < 0) ? 0 : 3;
    for (unsigned n = first; n < first + 3; n++) {
        float shape = 0;
        for (unsigned k = 0; k < 3; k++) {
            const float *a = p->anchor[k], *b = old->anchor[permutations[n][k]];
            shape = fmaxf(shape, fabsf(b[0] - a[0] - dx));
            shape = fmaxf(shape, fabsf(b[1] - a[1] - dy));
            shape = fmaxf(shape, fabsf(b[2] - a[2]));
            if (shape >= best)
                break;
        }
        if (shape < best) {
            best = shape;
            *order = n;
            if (best == 0)
                break;
        }
    }
    return best < 0.04f ? best + (fabsf(dx) + fabsf(dy)) * 0.2f : 1;
}

/* Buckets store indices so growing the corner array preserves their links. */
static unsigned corner_bucket(int x, int y) {
    return ((unsigned)x * 73856093u ^ (unsigned)y * 19349663u) % CORNER_BUCKETS;
}
/* Independently projected terrain patches can overlap at their borders.
 * On a slope their corner depths differ even on the same plane.
 * Compare at the other corner and allow for quarter-pixel RDP coordinates. */
static bool same_depth(float dz, float dx, float dy, const float gradient[2]) {
    float residual = dz - (dx * gradient[0] + dy * gradient[1]);
    float rounding = 0.25f * (fabsf(gradient[0]) + fabsf(gradient[1]));
    return fabsf(residual) <= 0.00025f + rounding;
}
static unsigned shared_corner(Interpolation *i, const float *p, const CornerSurface *surface, float sx, float sy) {
    float radius = surface->radius;
    float x = p[0] * sx, y = p[1] * sy;
    int left = (int)floorf((x - radius) / 4), right = (int)floorf((x + radius) / 4);
    int low = (int)floorf((y - radius) / 4), high = (int)floorf((y + radius) / 4);
    float best = radius * radius;
    unsigned nearest = 0;
    for (int row = low; row <= high; row++)
        for (int col = left; col <= right; col++)
            for (unsigned j = i->buckets[corner_bucket(col, row)]; j; j = i->corners[j - 1].next) {
                const Corner *c = &i->corners[j - 1];
                if (c->surface.depth != surface->depth)
                    continue;
                float dx = (c->position[0] - p[0]) * sx, dy = (c->position[1] - p[1]) * sy;
                float dist = dx * dx + dy * dy;
                float dz = c->position[2] - p[2];
                if (dist >= best)
                    continue;
                bool coincident = dist < 0.75f * 0.75f && fabsf(dz) <= 0.00025f;
                bool same_patch = surface->texture && surface->texture == c->surface.texture &&
                                  dist < c->surface.radius * c->surface.radius &&
                                  same_depth(dz, dx, dy, surface->gradient) &&
                                  same_depth(dz, dx, dy, c->surface.gradient);
                if (coincident || same_patch) {
                    best = dist;
                    nearest = j;
                }
            }
    return nearest;
}

static void weld_motion(Interpolation *i, const TrianglePose *pose, bool matched, float motion[3][3], float sx,
                        float sy) {
    CornerSurface surface = {.depth = (pose->flags & 0x01000000u) != 0, .radius = 0.75f};
    if (surface.depth && pose->texture) {
        surface.texture = pose->texture;
        const float *a = pose->anchor[0], *b = pose->anchor[1], *c = pose->anchor[2];
        float bz = b[2] - a[2], cz = c[2] - a[2];
        /* Window Z is affine. Express its gradient in native pixel units. */
        surface.gradient[0] = cross(bz, b[1] - a[1], cz, c[1] - a[1]) / pose->area / sx;
        surface.gradient[1] = cross(b[0] - a[0], bz, c[0] - a[0], cz) / pose->area / sy;
        /* Limit the wider search to three pixels and 1/16 of the shortest
         * edge on both patches, so small features retain the tight weld. */
        float radius_squared = 3.0f * 3.0f;
        for (unsigned k = 0; k < 3; k++) {
            const float *v = pose->anchor[k], *w = pose->anchor[(k + 1) % 3];
            float dx = (w[0] - v[0]) * sx, dy = (w[1] - v[1]) * sy;
            radius_squared = fminf(radius_squared, (dx * dx + dy * dy) / (16 * 16));
        }
        surface.radius = fmaxf(surface.radius, sqrtf(radius_squared));
    }
    unsigned existing[3], pins = 0;
    float sum[3] = {0};
    for (unsigned k = 0; k < 3; k++) {
        existing[k] = shared_corner(i, pose->anchor[k], &surface, sx, sy);
        if (existing[k]) {
            const float *pinned = i->corners[existing[k] - 1].delta;
            for (unsigned axis = 0; axis < 3; axis++) {
                motion[k][axis] = pinned[axis];
                sum[axis] += pinned[axis];
            }
            pins++;
        }
    }
    if (i->corner_count + 3 > i->corner_capacity) {
        unsigned cap = i->corner_capacity ? i->corner_capacity * 2 : 256;
        Corner *corners = realloc(i->corners, cap * sizeof(*corners));
        if (!corners)
            recomp_fail("GLES corner history allocation failed", 0);
        i->corners = corners;
        i->corner_capacity = cap;
    }
    for (unsigned k = 0; k < 3; k++) {
        if (existing[k])
            continue;
        if (!matched && pins)
            for (unsigned axis = 0; axis < 3; axis++)
                motion[k][axis] = sum[axis] / pins;
        const float *p = pose->anchor[k];
        unsigned bucket = corner_bucket((int)floorf(p[0] * sx / 4), (int)floorf(p[1] * sy / 4));
        Corner *c = &i->corners[i->corner_count++];
        c->surface = surface;
        c->next = i->buckets[bucket];
        i->buckets[bucket] = i->corner_count;
        memcpy(c->position, p, sizeof(c->position));
        memcpy(c->delta, motion[k], sizeof(c->delta));
    }
}

void recomp_gles2_midpoint_triangle(RecompRDPGles2 *s, uint32_t flags, unsigned start) {
    Interpolation *i = s->interpolation;
    FramePoses *cur = &i->current;
    unsigned count = s->batch_count - start;
    if (!count || i->ineligible)
        return;
    memcpy(i->batch + start, s->batch + start, count * sizeof(Vertex));
    if (cur->count >= MAX_POSES) {
        i->ineligible = true;
        return;
    }
    if (cur->count == cur->capacity) {
        size_t cap = cur->capacity ? cur->capacity * 2 : 128;
        TrianglePose *p = realloc(cur->poses, cap * sizeof(*p));
        if (!p)
            recomp_fail("GLES triangle history allocation failed", 0);
        cur->poses = p;
        cur->capacity = cap;
    }
    size_t slot = cur->count++;
    TrianglePose *p = &cur->poses[slot];
    p->mux0 = s->mux0;
    p->mux1 = s->mux1;
    p->mode = s->other_low;
    p->other_high = s->other_high;
    p->flags = flags & ~0x00800000u;
    p->texture = s->texture_enabled ? s->tmem_source[s->tiles[s->texture_tile].tmem] : 0;
    triangle_basis(p, s->batch + start, count, (flags & 0x00800000u) != 0);
    p->used = fabsf(p->area) < 1e-8f;
    size_t n = i->previous.count;
    if (p->used || !n)
        return;
    TrianglePose *old = NULL;
    unsigned order = 0;
    if (slot < n && pose_error(p, &i->previous.poses[slot], &order) == 0) {
        old = &i->previous.poses[slot];
    } else {
        float best = 1, second = 1;
        size_t end = slot + 49 < n ? slot + 49 : n;
        for (size_t k = slot > 48 ? slot - 48 : 0; k < end; k++) {
            TrianglePose *candidate = &i->previous.poses[k];
            unsigned candidate_order = 0;
            float score = pose_error(p, candidate, &candidate_order);
            if (score < best) {
                second = best;
                best = score;
                old = candidate;
                order = candidate_order;
            } else if (score < second)
                second = score;
        }
        if (second - best <= best * 0.2f)
            old = NULL;
    }
    float motion[3][3] = {{0}};
    if (old) {
        old->used = true;
        i->matched++;
        for (unsigned k = 0; k < 3; k++)
            for (unsigned axis = 0; axis < 3; axis++)
                motion[k][axis] = old->anchor[permutations[order][k]][axis] - p->anchor[k][axis];
    }
    weld_motion(i, p, old != NULL, motion, s->width * 0.5f, s->height * 0.5f);
    const float *a = p->anchor[0], *b = p->anchor[1], *c = p->anchor[2];
    for (unsigned k = 0; k < count; k++) {
        float *q = i->batch[start + k].position;
        float x = q[0] - a[0], y = q[1] - a[1];
        float u = cross(x, y, c[0] - a[0], c[1] - a[1]) / p->area;
        float v = cross(b[0] - a[0], b[1] - a[1], x, y) / p->area;
        for (unsigned axis = 0; axis < 3; axis++) {
            float delta = motion[0][axis] * (1 - u - v) + motion[1][axis] * u + motion[2][axis] * v;
            q[axis] += delta * 0.5f;
            i->moved |= fabsf(delta) > 0.0001f;
        }
    }
}
const Vertex *recomp_gles2_midpoint_batch(RecompRDPGles2 *s) {
    return s->interpolation->batch;
}
void recomp_gles2_end_interpolation(RecompRDPGles2 *s) {
    Interpolation *i = s->interpolation;
    if (!s->full_sync)
        return;
    i->in_frame = false;
    if (i->ineligible) {
        i->current.count = 0;
        return;
    }
    if (!i->target)
        return;
    Surface *mid = &s->midpoints[i->target - s->surfaces];
    i->target->has_midpoint = mid->serial == i->serial && i->moved && i->matched * 4 >= i->current.count * 3 &&
                              i->matched * 4 >= i->previous.count * 3;
    mid->lights = i->target->lights;
}
Surface *recomp_gles2_interpolated_display(RecompRDPGles2 *s, Surface *f) {
    Interpolation *i = s->interpolation;
    /* At native 60 Hz, consecutive source frames each own one display tick.
     * A repeated scanout establishes room for a midpoint on the next change. */
    if (f == &s->cpu_image || !f->serial) {
        i->ticks = 0;
        i->displayed = 0;
    } else if (i->displayed == f->serial) {
        if (i->ticks < 2)
            i->ticks++;
    } else {
        bool show = i->ticks >= 2 && i->displayed + 1 == f->serial && f->has_midpoint;
        i->displayed = f->serial;
        i->ticks = 1;
        if (show)
            return &s->midpoints[f - s->surfaces];
    }
    return f;
}
