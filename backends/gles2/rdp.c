#include "internal.h"
#include "rdp_triangle.h"
#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static void rgba(uint32_t bits, float *c) {
    for (unsigned i = 0; i < 4; i++)
        c[i] = (float)((bits >> (24 - i * 8)) & 255) / 255;
}
static void flush(RecompRDPGles2 *s, const float uv_bounds[4]) {
    if (!s->batch_count)
        return;
    Program *p = recomp_gles2_combiner(s);
    s->gl.UseProgram(p->id);
    s->gl.Uniform4fv(p->uv_bounds, 1, uv_bounds);
    s->gl.Uniform4fv(p->prim, 1, s->prim);
    s->gl.Uniform4fv(p->env, 1, s->env);
    s->gl.Uniform4fv(p->fog, 1, s->fog_color);
    s->gl.Uniform1f(p->lod, s->prim_lod);
    s->gl.Uniform1f(p->perspective, s->rdp_perspective ? 1.0f : 0.0f);
    s->gl.Uniform1f(p->alpha, (s->other_low & 3) ? (float)(s->blend_color & 255) / 255 : -1);
    /* Full-pixel coverage starts at eight samples. CVG_X_ALPHA only drops
     * fragments when multiplying those samples by final alpha yields < 1. */
    s->gl.Uniform1f(p->coverage_alpha, (s->other_low & 0x1000) ? 0.125f : -1);
    s->gl.Uniform1f(p->fogen, s->fog ? 1.0f : 0.0f);
    for (unsigned i = 0; i < 2; i++) {
        Tile *t = &s->tiles[(s->texture_tile + (p->cycle == 0 ? 0 : i)) & 7];
        s->gl.ActiveTexture(GL_TEXTURE0 + i);
        if (!s->texture_enabled || !p->texture[i]) {
            s->active_texture[i] = s->white;
            s->tex_width[i] = s->tex_height[i] = 1;
        } else if (s->textures_dirty)
            s->active_texture[i] = recomp_gles2_texture(s, t, i);
        s->gl.BindTexture(GL_TEXTURE_2D, s->active_texture[i]);
        GLint filter = p->cycle == 2 ? GL_NEAREST : GL_LINEAR;
        GLint min_filter = filter;
        if (p->cycle != 2 && s->mipmaps && s->texture_enabled && p->texture[i])
            min_filter = GL_LINEAR_MIPMAP_LINEAR;
        s->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, min_filter);
        s->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        float dim[4] = {(float)s->tex_width[i], (float)s->tex_height[i],
                        t->ss > 10 ? (float)(1u << (16 - t->ss)) : 1.0f / (float)(1u << t->ss),
                        t->st > 10 ? (float)(1u << (16 - t->st)) : 1.0f / (float)(1u << t->st)};
        float origin[4] = {(float)t->sl / 4, (float)t->tl / 4, 0, 0};
        float wrap[4] = {t->ms && !(t->cs & 2) ? (float)(1u << t->ms) : 0,
                         t->mt && !(t->ct & 2) ? (float)(1u << t->mt) : 0, (float)(t->cs & 1), (float)(t->ct & 1)};
        /* Let the sampler interpolate across complete repeating tiles. Manual
         * wrapping is still needed for masks covering only part of a texture. */
        for (unsigned axis = 0; axis < 2; axis++) {
            bool repeat = wrap[axis] > 0 && wrap[axis] == dim[axis];
            origin[2 + axis] = repeat ? 1.0f : 0.0f;
            s->gl.TexParameteri(GL_TEXTURE_2D, axis ? GL_TEXTURE_WRAP_T : GL_TEXTURE_WRAP_S,
                                repeat ? (wrap[2 + axis] > 0 ? GL_MIRRORED_REPEAT : GL_REPEAT) : GL_CLAMP_TO_EDGE);
        }
        s->gl.Uniform4fv(p->tile[i][0], 1, dim);
        s->gl.Uniform4fv(p->tile[i][1], 1, origin);
        s->gl.Uniform4fv(p->tile[i][2], 1, wrap);
    }
    s->textures_dirty = false;
    bool compare_depth = s->depth_test && (s->other_low & 0x10);
    bool write_depth = s->depth_test && (s->other_low & 0x20);
    /* RDP comparison and update are independent. Disabling the GL test also
     * disables writes, so update-only passes must use an enabled ALWAYS test. */
    if (compare_depth || write_depth)
        s->gl.Enable(GL_DEPTH_TEST);
    else
        s->gl.Disable(GL_DEPTH_TEST);
    s->gl.DepthFunc(compare_depth ? GL_LEQUAL : GL_ALWAYS);
    s->gl.DepthMask(write_depth ? GL_TRUE : GL_FALSE);
    bool force_blend = s->other_low & 0x4000;
    if (s->screen_effects) {
        bool geometry = s->depth_test && !s->drawing_rectangle && s->color_siz >= 2;
        if (geometry && s->torch_mode && s->surface->light_serial != s->light_serial) {
            s->surface->lights = s->task_lights;
            s->surface->light_serial = s->light_serial;
        }
        s->gl.Uniform1i(p->record_depth, geometry);
        s->gl.Uniform1i(p->blended, force_blend);
    }
    unsigned blender_p = (s->other_low >> (p->cycle == 1 ? 28 : 30)) & 3;
    /* Without forced blending, the final blender cycle outputs P. When P
     * selects framebuffer memory, keep the old color but still update Z. */
    if (p->cycle < 2 && !force_blend && blender_p == 1) {
        s->gl.Enable(GL_BLEND);
        s->gl.BlendFunc(GL_ZERO, GL_ONE);
    } else if (force_blend) {
        s->gl.Enable(GL_BLEND);
        s->gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else
        s->gl.Disable(GL_BLEND);
    s->gl.Disable(GL_CULL_FACE); /* RSP has already culled/clipped geometry. */
    float scale = (float)s->render_scale;
    s->gl.Viewport((GLint)(s->viewport[0] * scale),
                   (GLint)(((float)s->height - s->viewport[1] - s->viewport[3]) * scale),
                   (GLsizei)(s->viewport[2] * scale), (GLsizei)(s->viewport[3] * scale));
    s->gl.Enable(GL_SCISSOR_TEST);
    s->gl.Scissor((GLint)(s->scissor[0] * s->render_scale),
                  (GLint)(((int)s->height - (int)s->scissor[3]) * (int)s->render_scale),
                  (GLsizei)((s->scissor[2] - s->scissor[0]) * s->render_scale),
                  (GLsizei)((s->scissor[3] - s->scissor[1]) * s->render_scale));
    s->gl.BindBuffer(GL_ARRAY_BUFFER, s->buffer);
    s->gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(s->batch_count * sizeof(Vertex)), s->batch, GL_STREAM_DRAW);
    for (unsigned i = 0; i < 5; i++)
        s->gl.EnableVertexAttribArray(i);
    s->gl.VertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, position));
    s->gl.VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, color));
    s->gl.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, uv));
    s->gl.VertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, fog));
    s->gl.VertexAttribPointer(4, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, reciprocal));
    if (s->wireframe && !s->drawing_rectangle && p->cycle != 3)
        recomp_gles2_draw_wireframe(s, p);
    else
        s->gl.DrawArrays(GL_TRIANGLES, 0, (GLsizei)s->batch_count);

    s->color_dirty = true;
    s->batch_count = 0;
}

void recomp_gles2_flush(RecompRDPGles2 *s) {
    static const float unbounded[4] = {-FLT_MAX, -FLT_MAX, FLT_MAX, FLT_MAX};
    flush(s, unbounded);
}

static void rectangle(RecompRDPGles2 *s, float x0, float y0, float x1, float y1, float u, float v, float du, float dv,
                      bool flip) {
    recomp_gles2_flush(s);
    bool perspective = s->rdp_perspective;
    s->rdp_perspective = false;
    float oldviewport[4];
    memcpy(oldviewport, s->viewport, sizeof(oldviewport));
    s->viewport[0] = s->viewport[1] = 0;
    s->viewport[2] = (float)s->width;
    s->viewport[3] = (float)s->height;
    bool depth = s->depth_test, fog = s->fog;
    /* Rectangles lack interpolated Z, but one- and two-cycle draws can use
     * primitive Z. Keep compare/update independently gated by OtherMode. */
    bool primitive_z = ((s->other_high >> 20) & 3) < 2 && (s->other_low & 4);
    s->depth_test = primitive_z;
    s->fog = false;
    /* S/T name the first native pixel's sample. GLES interpolates from
     * rectangle edges, half a pixel before that sample on each axis. */
    float last_u = u + fmaxf((flip ? y1 - y0 : x1 - x0) - 1, 0) * du;
    float last_v = v + fmaxf((flip ? x1 - x0 : y1 - y0) - 1, 0) * dv;
    float uv_bounds[4] = {fminf(u, last_u), fminf(v, last_v), fmaxf(u, last_u), fmaxf(v, last_v)};
    u -= 0.5f * du;
    v -= 0.5f * dv;
    Vertex q[4] = {0};
    float x[4] = {x0, x1, x1, x0}, y[4] = {y0, y0, y1, y1};
    for (unsigned i = 0; i < 4; i++) {
        q[i].position[0] = x[i] * 2 / (float)s->width - 1;
        q[i].position[1] = 1 - y[i] * 2 / (float)s->height;
        q[i].position[3] = 1;
        q[i].position[2] = primitive_z ? s->primitive_depth * 2 - 1 : 0;
        /* RDP rectangles supply zero shade coefficients. */
        q[i].uv[0] = u + (flip ? y[i] - y0 : x[i] - x0) * du;
        q[i].uv[1] = v + (flip ? x[i] - x0 : y[i] - y0) * dv;
    }
    const unsigned order[] = {0, 1, 2, 0, 2, 3};
    for (unsigned i = 0; i < 6; i++)
        s->batch[s->batch_count++] = q[order[i]];

    s->drawing_rectangle = true;
    flush(s, uv_bounds);
    s->drawing_rectangle = false;
    s->depth_test = depth;
    s->fog = fog;
    memcpy(s->viewport, oldviewport, sizeof(oldviewport));
    s->rdp_perspective = perspective;
}

static void texture_rectangle(RecompRDPGles2 *s, uint32_t w0, uint32_t w1, uint32_t coordinates, uint32_t derivatives,
                              bool flip) {
    unsigned cycle = (s->other_high >> 20) & 3;
    float x0 = (float)((w1 >> 12) & 4095) / 4, y0 = (float)(w1 & 4095) / 4, x1 = (float)((w0 >> 12) & 4095) / 4,
          y1 = (float)(w0 & 4095) / 4;
    float du = (float)(int16_t)(derivatives >> 16) / 1024, dv = (float)(int16_t)derivatives / 1024;
    if (cycle == 2) {
        du /= 4;
        x1 = floorf(x1) + 1;
        y1 = floorf(y1) + 1;
    }
    unsigned tile = s->texture_tile;
    s->texture_tile = (w1 >> 24) & 7;
    bool enabled = s->texture_enabled;
    s->texture_enabled = true;
    s->textures_dirty = true;
    float u = (float)(int16_t)(coordinates >> 16) / 32, v = (float)(int16_t)coordinates / 32;
    rectangle(s, x0, y0, x1, y1, u, v, du, dv, flip);
    s->texture_enabled = enabled;
    s->texture_tile = tile;
    s->textures_dirty = true;
}
static bool consume_rdp(RecompRDPGles2 *s, uint32_t w0, uint32_t w1) {
    unsigned op = w0 >> 24;
    if (op != 0xc0)
        recomp_gles2_flush(s);
    switch (op) {
        case 0xc0:
        case 0xe6:
        case 0xe7:
        case 0xe8:
            return true;
        case 0xe9:
            s->full_sync = true;
            return true;
        case 0xee:
            s->primitive_depth = (float)((w1 >> 16) & 0x7fff) / 32768;
            return true;
        case 0xef:
            s->other_high = w0 & 0xffffff;
            s->other_low = w1;
            s->textures_dirty = true;
            return true;
        case 0xed:
            s->scissor[0] = ((w0 >> 12) & 4095) / 4;
            s->scissor[1] = (w0 & 4095) / 4;
            s->scissor[2] = ((w1 >> 12) & 4095) / 4;
            s->scissor[3] = (w1 & 4095) / 4;
            return s->scissor[2] >= s->scissor[0] && s->scissor[3] >= s->scissor[1];
        case 0xf5: {
            Tile *t = &s->tiles[(w1 >> 24) & 7];
            t->fmt = (w0 >> 21) & 7;
            t->siz = (w0 >> 19) & 3;
            t->line = (w0 >> 9) & 511;
            t->tmem = w0 & 511;
            t->palette = (w1 >> 20) & 15;
            t->ct = (w1 >> 18) & 3;
            t->mt = (w1 >> 14) & 15;
            t->st = (w1 >> 10) & 15;
            t->cs = (w1 >> 8) & 3;
            t->ms = (w1 >> 4) & 15;
            t->ss = w1 & 15;
            s->textures_dirty = true;
            return true;
        }
        case 0xf2: {
            Tile *t = &s->tiles[(w1 >> 24) & 7];
            t->sl = (w0 >> 12) & 4095;
            t->tl = w0 & 4095;
            t->sh = (w1 >> 12) & 4095;
            t->th = w1 & 4095;
            s->textures_dirty = true;
            return true;
        }
        case 0xf3:
        case 0xf4:
        case 0xf0:
            recomp_gles2_load_tile(s, &s->tiles[(w1 >> 24) & 7], w0, w1, op == 0xf3, op == 0xf0);
            return true;
        case 0xfd:
            s->image_fmt = (w0 >> 21) & 7;
            s->image_siz = (w0 >> 19) & 3;
            s->image_width = (w0 & 4095) + 1;
            s->image_address = w1;
            return s->image_fmt != 1;
        case 0xfc:
            s->mux0 = w0 & 0xffffff;
            s->mux1 = w1;
            s->textures_dirty = true;
            return true;
        case 0xfa:
            rgba(w1, s->prim);
            s->prim_lod = (float)(w0 & 255) / 255;
            return true;
        case 0xfb:
            rgba(w1, s->env);
            return true;
        case 0xf8:
            rgba(w1, s->fog_color);
            return true;
        case 0xf9:
            s->blend_color = w1;
            return true;
        case 0xf7:
            s->fill_color = w1;
            return true;
        case 0xfe:
            s->depth_address = span(s, w1, 1);
            if (!s->clearing_depth)
                recomp_gles2_bind_surface(s);
            return true;
        case 0xff: {
            unsigned siz = (w0 >> 19) & 3;
            unsigned fmt = (w0 >> 21) & 7;
            if (!(fmt == 0 && siz >= 2) && !((fmt == 2 || fmt == 4) && siz == 1))
                return false;
            unsigned width = (w0 & 4095) + 1, height = 0;
            for (size_t i = 0; i < s->framebuffer_size_count; i++)
                if (s->framebuffer_sizes[i].width == width)
                    height = s->framebuffer_sizes[i].height;
            if (!height)
                return false;
            uint32_t address = span(s, w1, 1);
            bool depth = siz == 2 && address == s->depth_address;
            bool changed = !s->color_image_set || address != s->color_address || siz != s->color_siz ||
                           s->surface->width != width * s->render_scale ||
                           s->surface->height != height * s->render_scale;
            /* Coverage passes read the previous RGBA image through TMEM, then
             * leave their I8 output in RAM before restoring the display target. */
            if (s->color_image_set && changed && s->color_dirty && (s->color_siz == 1 || (!depth && siz == 1)))
                recomp_gles2_readback_color(s);
            if (!depth && changed)
                s->color_dirty = false;
            s->width = width;
            s->height = height;
            s->clearing_depth = depth;
            if (!s->clearing_depth) {
                s->color_image_set = true;
                s->color_address = address;
                s->color_siz = siz;
                recomp_gles2_bind_surface(s);
                if (changed && siz == 1)
                    recomp_gles2_upload_i8(s, 0, 0, width, height, false);
            }
            return true;
        }
        case 0xf6: {
            unsigned x0 = ((w1 >> 12) & 4095) / 4, y0 = (w1 & 4095) / 4, x1 = ((w0 >> 12) & 4095) / 4,
                     y1 = (w0 & 4095) / 4, cycle = (s->other_high >> 20) & 3;
            if (cycle >= 2) {
                x1++;
                y1++;
            }
            if (s->clearing_depth) {
                recomp_gles2_depth_surface(s, s->width * s->render_scale, s->height * s->render_scale)->clear = true;
                return true;
            }
            if (cycle == 3) {
                uint8_t p[4];
                if (s->color_siz == 1) {
                    recomp_gles2_upload_i8(s, x0, y0, x1, y1, true);
                    s->color_dirty = true;
                    return true;
                }
                if (s->color_siz == 3)
                    for (unsigned i = 0; i < 4; i++)
                        p[i] = (uint8_t)(s->fill_color >> (24 - i * 8));
                else
                    pixel16(s->fill_color & 65535, p, false);
                s->gl.Enable(GL_SCISSOR_TEST);
                s->gl.Scissor((GLint)(x0 * s->render_scale), (GLint)(((int)s->height - (int)y1) * (int)s->render_scale),
                              (GLsizei)((x1 - x0) * s->render_scale), (GLsizei)((y1 - y0) * s->render_scale));
                s->gl.ClearColor((float)p[0] / 255, (float)p[1] / 255, (float)p[2] / 255, (float)p[3] / 255);
                s->gl.Clear(GL_COLOR_BUFFER_BIT);
                if (s->surface->scene_depth) {
                    const GLfloat empty[4] = {0, 0, 0, 1};
                    s->gl.ClearBufferfv(GL_COLOR, 1, empty);
                }
                s->color_dirty = true;

            } else {
                bool enabled = s->texture_enabled;
                s->texture_enabled = false;
                rectangle(s, (float)x0, (float)y0, (float)x1, (float)y1, 0, 0, 0, 0, false);
                s->texture_enabled = enabled;
                s->textures_dirty = true;
            }
            return true;
        }
        case 0xea:
        case 0xeb:
        case 0xec:
            return true; /* Key/convert constants are unused by SM64 RGBA/IA materials. */
        default:
            fprintf(stderr, "GLES2 unsupported graphics command %02x at %08x (%08x %08x)\n", op, 0, w0, w1);
            return false;
    }
}

static bool rdp_triangle_mesh(RecompRDPGles2 *s, const uint32_t *words, size_t count) {
    RecompRdpTriangle mesh;
    if (!recomp_rdp_triangle_decode(words, count, &mesh))
        return false;
    uint32_t mode = s->other_low;
    bool fog = ((mode >> 30) & 3) == 3 && ((mode >> 22) & 3) == 0 && ((mode >> 18) & 3) == 0;
    unsigned fog_source = (mode >> 26) & 3;
    fog = fog && ((s->other_high >> 20) & 3) == 1 && (fog_source == 1 || fog_source == 2);
    bool perspective = mesh.texture && (s->other_high & 0x80000);
    bool changed = s->rdp_perspective != perspective || s->depth_test != mesh.depth || s->fog != fog ||
                   s->texture_tile != mesh.tile || s->texture_enabled != mesh.texture || s->viewport[0] ||
                   s->viewport[1] || s->viewport[2] != (float)s->width || s->viewport[3] != (float)s->height;
    if (changed || s->batch_count + mesh.count > RECOMP_GLES2_BATCH_VERTICES)
        recomp_gles2_flush(s);
    s->viewport[0] = s->viewport[1] = 0;
    s->viewport[2] = (float)s->width;
    s->viewport[3] = (float)s->height;
    s->rdp_perspective = perspective;
    s->depth_test = mesh.depth;
    s->fog = fog;
    s->texture_tile = mesh.tile;
    s->texture_enabled = mesh.texture;
    s->textures_dirty |= changed;
    /* GLES2 has no portable access to the stored RDP depth/dz pair. Bias
     * read-only decals by their native tolerance so separately quantized
     * coplanar planes stay visible, independent of render scale/GL depth bits.
     * Updating triangles retain their original depth values. */
    float bias = mesh.depth && (mode & 0xc00) == 0xc00 && !(mode & 0x20) ? mesh.depth_delta : 0;
    for (unsigned i = 0; i < mesh.count; i++) {
        const RecompRdpVertex *source = &mesh.vertices[i];
        Vertex *v = &s->batch[s->batch_count++];
        *v = (Vertex){0};
        float w = source->position[3];
        v->position[0] = (source->position[0] * 2 / (float)s->width - 1) * w;
        v->position[1] = (1 - source->position[1] * 2 / (float)s->height) * w;
        float z = source->position[2];
        if (bias)
            z = fmaxf(0, z - bias);
        v->position[2] = (z * 2 - 1) * w;
        v->position[3] = w;
        memcpy(v->color, source->color, sizeof(v->color));
        v->fog = fog_source == 1 ? s->fog_color[3] : source->color[3];
        memcpy(v->uv, source->uv, sizeof(v->uv));
        v->reciprocal = source->reciprocal;
    }

    return true;
}

static void draw_begin(RecompRDPGles2 *s) {
#ifdef RECOMP_GLES2_TEXTURE_PACKS
    recomp_gles2_stream_replacements(s);
#endif
    s->full_sync = false;
    s->started = true;
    recomp_gles2_bind_surface(s);
}
size_t recomp_gles2_command_words(uint32_t w0, size_t available) {
    w0 |= 0xc0000000u;
    unsigned op = w0 >> 24;
    size_t n = op >= 0xc8 && op <= 0xcf ? recomp_rdp_triangle_words(w0) : (op == 0xe4 || op == 0xe5 ? 4 : 2);
    if (n > available)
        recomp_fail("truncated RDP command", w0);
    return n;
}
void recomp_rdp_gles2_draw_rdp(RecompRDPGles2 *s, uint8_t *ram, size_t ram_size, const uint32_t *words, size_t count) {
    recomp_gles2_require_owner(s);
    if (!ram || ram_size != s->rt->rdram_size || (!words && count) || count > 4 * 1024 * 1024)
        recomp_fail("invalid native RSP RDP output", 0);
    s->draw_ram = ram;
    s->rdp_perspective = false;
    draw_begin(s);
    for (size_t offset = 0; offset < count;) {
        size_t length = recomp_gles2_command_words(words[offset], count - offset);
        uint32_t w0 = words[offset] | 0xc0000000u, w1 = words[offset + 1];
        unsigned op = w0 >> 24;
        if (op >= 0xc8 && op <= 0xcf) {
            uint32_t packet[RECOMP_RDP_TRIANGLE_WORDS];
            memcpy(packet, words + offset, length * sizeof(*packet));
            packet[0] = w0;
            if (!rdp_triangle_mesh(s, packet, length))
                recomp_fail("invalid RDP triangle coefficients", (uint32_t)offset * 4);
        } else {
            /* RSP kernels often re-emit the current mode before geometry. A
             * redundant write must not split a batch with identical state. */
            bool redundant = (op == 0xef && s->other_high == (w0 & 0xffffff) && s->other_low == w1) ||
                             (op == 0xfc && s->mux0 == (w0 & 0xffffff) && s->mux1 == w1);
            if (op == 0xe4 || op == 0xe5) {
                texture_rectangle(s, w0, w1, words[offset + 2], words[offset + 3], op == 0xe5);
            } else if (!redundant && !consume_rdp(s, w0, w1)) {
                fprintf(stderr, "RDP command %08x %08x at word %zu\n", w0, w1, offset);
                recomp_fail("unsupported RDP command", (uint32_t)offset * 4);
            }
        }
        offset += length;
    }
    recomp_gles2_flush(s);
    if (s->full_sync) {
        if (s->color_image_set && (!s->ram_scanout || s->color_dirty))
            recomp_gles2_readback_color(s);
        else
            s->gl.Finish();
    }
    s->draw_ram = s->rt->rdram;
}
bool recomp_rdp_gles2_full_sync(RecompRDPGles2 *s) {
    return s->full_sync;
}
