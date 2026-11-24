#include "internal.h"
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void recomp_gles2_create_surface(RecompRDPGles2 *s, Surface *f, unsigned width, unsigned height) {
    f->valid = true;
    f->width = width;
    f->height = height;
    s->gl.GenTextures(1, &f->texture);
    s->gl.BindTexture(GL_TEXTURE_2D, f->texture);
    s->gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width, (GLsizei)height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    s->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    s->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    s->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    s->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    s->gl.GenFramebuffers(1, &f->framebuffer);
    s->gl.BindFramebuffer(GL_FRAMEBUFFER, f->framebuffer);
    s->gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, f->texture, 0);
    if (s->gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        recomp_fail("GLES2 framebuffer incomplete", f->address);
    s->gl.Disable(GL_SCISSOR_TEST);
    s->gl.DepthMask(GL_TRUE);
    s->gl.ClearColor(0, 0, 0, 1);
    s->gl.Clear(GL_COLOR_BUFFER_BIT);
    s->textures_dirty = true;
}
void recomp_gles2_destroy_surface(RecompRDPGles2 *s, Surface *f) {
    if (!f->valid)
        return;
    s->gl.DeleteTextures(1, &f->texture);
    s->gl.DeleteFramebuffers(1, &f->framebuffer);
    *f = (Surface){0};
}
static Surface *surface(RecompRDPGles2 *s, uint32_t address) {
    Surface *f = NULL;
    for (unsigned i = 0; i < 8; i++)
        if (s->surfaces[i].valid && s->surfaces[i].address == address) {
            f = &s->surfaces[i];
            if (f->width == s->width * s->render_scale && f->height == s->height * s->render_scale)
                return f;
            recomp_gles2_destroy_surface(s, f);
            break;
        }
    for (unsigned i = 0; i < 8; i++)
        if (!s->surfaces[i].valid) {
            f = &s->surfaces[i];
            break;
        }
    if (!f)
        recomp_fail("GLES2 framebuffer cache exhausted", address);
    f->address = address;
    recomp_gles2_create_surface(s, f, s->width * s->render_scale, s->height * s->render_scale);
    return f;
}
/* N64 color images can rotate while retaining the same physical Z image.
 * Keep depth storage independent of color FBO ownership and task boundaries. */
DepthSurface *recomp_gles2_depth_surface(RecompRDPGles2 *s, unsigned width, unsigned height) {
    DepthSurface *available = NULL;
    for (unsigned i = 0; i < 16; i++) {
        DepthSurface *depth = &s->depths[i];
        if (depth->buffer && depth->address == s->depth_address && depth->width == width && depth->height == height)
            return depth;
        if (!depth->buffer) {
            if (!available)
                available = depth;
            continue;
        }
        bool attached = false;
        for (unsigned j = 0; j < 8; j++)
            attached |= s->surfaces[j].valid && s->surfaces[j].depth == depth->buffer;
        if (!available && !attached && !depth->clear)
            available = depth;
    }
    if (!available)
        recomp_fail("GLES2 depth-image cache exhausted", s->depth_address);
    if (available->buffer)
        s->gl.DeleteRenderbuffers(1, &available->buffer);
    *available = (DepthSurface){.address = s->depth_address, .width = width, .height = height, .clear = true};
    s->gl.GenRenderbuffers(1, &available->buffer);
    s->gl.BindRenderbuffer(GL_RENDERBUFFER, available->buffer);
    s->gl.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24_OES, (GLsizei)width, (GLsizei)height);
    return available;
}
void recomp_gles2_bind_surface(RecompRDPGles2 *s) {
    s->surface = surface(s, s->color_address);
    s->gl.BindFramebuffer(GL_FRAMEBUFFER, s->surface->framebuffer);
    DepthSurface *depth = recomp_gles2_depth_surface(s, s->surface->width, s->surface->height);
    if (s->surface->depth != depth->buffer) {
        s->surface->depth = depth->buffer;
        s->gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth->buffer);
        if (s->gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
            recomp_fail("GLES2 depth attachment incomplete", s->depth_address);
    }
    if (depth->clear) {
        s->gl.Disable(GL_SCISSOR_TEST);
        s->gl.DepthMask(GL_TRUE);
        s->gl.Clear(GL_DEPTH_BUFFER_BIT);
        depth->clear = false;
    }
}

static void blit_texture(RecompRDPGles2 *s, const Surface *source, GLuint framebuffer, int x, int y, int width,
                         int height, bool color_correct) {
    s->gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    s->gl.Disable(GL_SCISSOR_TEST);
    s->gl.Disable(GL_DEPTH_TEST);
    s->gl.Disable(GL_BLEND);
    s->gl.Disable(GL_CULL_FACE);
    s->gl.Viewport(x, y, width, height);
    s->gl.UseProgram(s->blit);
    s->gl.Uniform1f(s->blit_gamma, color_correct ? s->color_gamma : 1.0f);
    s->gl.Uniform1f(s->blit_aa, color_correct && s->antialias ? 1.0f : 0.0f);
    s->gl.Uniform2f(s->blit_pixel, 1.0f / (float)source->width, 1.0f / (float)source->height);
    s->gl.ActiveTexture(GL_TEXTURE0);
    s->gl.BindTexture(GL_TEXTURE_2D, source->texture);
    s->gl.Uniform1i(s->gl.GetUniformLocation(s->blit, "image"), 0);
    Vertex q[6] = {0};
    const float xy[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
    for (unsigned i = 0; i < 6; i++) {
        q[i].position[0] = xy[i][0];
        q[i].position[1] = xy[i][1];
        q[i].position[3] = 1;
        q[i].uv[0] = (xy[i][0] + 1) / 2;
        q[i].uv[1] = (xy[i][1] + 1) / 2;
    }
    s->gl.BindBuffer(GL_ARRAY_BUFFER, s->buffer);
    s->gl.BufferData(GL_ARRAY_BUFFER, sizeof(q), q, GL_STREAM_DRAW);
    s->gl.EnableVertexAttribArray(0);
    s->gl.EnableVertexAttribArray(2);
    s->gl.VertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, position));
    s->gl.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, uv));
    s->gl.DrawArrays(GL_TRIANGLES, 0, 6);
    s->textures_dirty = true;
}

/* Seed partial I8 draws from RAM. Fill mode repeats the four guest-order
 * bytes in the fill register at their absolute framebuffer byte positions. */
void recomp_gles2_upload_i8(RecompRDPGles2 *s, unsigned x0, unsigned y0, unsigned x1, unsigned y1, bool fill) {
    if (fill) {
        if (x0 < s->scissor[0])
            x0 = s->scissor[0];
        if (y0 < s->scissor[1])
            y0 = s->scissor[1];
        if (x1 > s->scissor[2])
            x1 = s->scissor[2];
        if (y1 > s->scissor[3])
            y1 = s->scissor[3];
    }
    if (x1 > s->width)
        x1 = s->width;
    if (y1 > s->height)
        y1 = s->height;
    if (x0 >= x1 || y0 >= y1)
        return;
    uint32_t base = span(s, s->color_address, (size_t)s->width * s->height);
    unsigned scale = s->render_scale, width = (x1 - x0) * scale, height = (y1 - y0) * scale;
    size_t stride = (size_t)width * 4;
    uint8_t *pixels = malloc(stride * height);
    if (!pixels)
        recomp_fail("GLES2 intensity upload allocation failed", base);
    for (unsigned y = y0; y < y1; ++y) {
        uint8_t *row = pixels + (y1 - 1 - y) * scale * stride;
        for (unsigned x = x0; x < x1; ++x) {
            uint32_t offset = base + y * s->width + x;
            uint8_t value = fill ? (uint8_t)(s->fill_color >> (24 - (offset & 3) * 8)) : (uint8_t)load(s, offset, 1);
            memset(row + (x - x0) * scale * 4, value, scale * 4);
        }
        for (unsigned i = 1; i < scale; ++i)
            memcpy(row + i * stride, row, stride);
    }
    s->gl.BindTexture(GL_TEXTURE_2D, s->surface->texture);
    s->gl.TexSubImage2D(GL_TEXTURE_2D, 0, (GLint)(x0 * scale), (GLint)((s->height - y1) * scale), (GLsizei)width,
                        (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    s->textures_dirty = true;
    free(pixels);
}

void recomp_gles2_readback_color(RecompRDPGles2 *s) {
    unsigned width = s->surface->width / s->render_scale, height = s->surface->height / s->render_scale;
    size_t pixels_count = (size_t)width * height;
    uint8_t *pixels = malloc(pixels_count * 4);
    if (!pixels)
        recomp_fail("GLES2 color readback allocation failed", s->color_address);
    unsigned bpp = 1u << (s->color_siz - 1);
    uint32_t target = recomp_ram_address_write_slow(s->rt, 0x80000000u | s->color_address, pixels_count * bpp);
    if (target & (bpp - 1))
        recomp_fail("unaligned GLES2 color image", s->color_address);
    if (s->render_scale > 1) {
        if (s->readback.valid && (s->readback.width != width || s->readback.height != height))
            recomp_gles2_destroy_surface(s, &s->readback);
        if (!s->readback.valid)
            recomp_gles2_create_surface(s, &s->readback, width, height);
        /* Resolve at native resolution before packing pixels into guest RAM. */
        blit_texture(s, s->surface, s->readback.framebuffer, 0, 0, (int)width, (int)height, false);
    }
    s->gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
    s->gl.ReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    for (unsigned y = 0; y < height; y++)
        for (unsigned x = 0; x < width; x++) {
            const uint8_t *p = pixels + ((size_t)(height - y - 1) * width + x) * 4;
            uint32_t offset = target + (y * width + x) * bpp;
            /* On 8-bit color targets the RDP stores red at even byte addresses and
             * green at odd addresses. Mirror the same guest lanes into both
             * the task snapshot and live memory, including partial words. */
            if (bpp == 1) {
                uint8_t value = p[offset & 1];
#if RECOMP_LITTLE_ENDIAN_HOST
                offset ^= 3;
#endif
                s->draw_ram[offset] = value;
                s->rt->rdram[offset] = value;
            } else if (bpp == 2) {
                uint16_t value = (uint16_t)((p[0] >> 3) << 11 | (p[1] >> 3) << 6 | (p[2] >> 3) << 1 | (p[3] >= 128));
#if RECOMP_LITTLE_ENDIAN_HOST
                offset ^= 2;
#endif
                memcpy(s->draw_ram + offset, &value, sizeof(value));
                memcpy(s->rt->rdram + offset, &value, sizeof(value));
            } else {
                uint32_t value = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
                memcpy(s->draw_ram + offset, &value, sizeof(value));
                memcpy(s->rt->rdram + offset, &value, sizeof(value));
            }
        }
    s->color_dirty = false;
    free(pixels);
    s->gl.BindFramebuffer(GL_FRAMEBUFFER, s->surface->framebuffer);
}

void recomp_rdp_gles2_readback_range(RecompRDPGles2 *s, uint32_t *address, size_t *bytes) {
    recomp_gles2_require_owner(s);
    *address = s->full_sync ? s->color_address : 0;
    *bytes = (s->full_sync && s->color_image_set)
                 ? (size_t)(s->surface->width / s->render_scale) * (s->surface->height / s->render_scale) *
                       (1u << (s->color_siz - 1))
                 : 0;
}
void recomp_rdp_gles2_set_framebuffer(RecompRDPGles2 *s, uint32_t address, uint32_t vi_status) {
    recomp_gles2_require_owner(s);
    s->present_address = address;
    if (!s->gamma_overridden) {
        /* Match RT64's VI gamma-enable bit and presentation exponent. */
        s->color_gamma = (vi_status & 0x08u) ? 1.0f / 2.2f : 1.0f;
    }
}
void recomp_rdp_gles2_set_ram_scanout(RecompRDPGles2 *s) {
    s->ram_scanout = true;
}
void recomp_rdp_gles2_set_framebuffer_sizes(RecompRDPGles2 *s, const RecompFramebufferSize *sizes, size_t count) {
    recomp_gles2_require_owner(s);
    if (s->started || !sizes || !count || count > 16)
        recomp_fail("configure GLES2 framebuffer extents before drawing", 0);
    for (size_t i = 0; i < count; i++) {
        if (!sizes[i].width || sizes[i].width > 1024 || !sizes[i].height || sizes[i].height > 1024)
            recomp_fail("invalid GLES2 framebuffer extent", (uint32_t)i);
        for (size_t j = 0; j < i; j++)
            if (sizes[j].width == sizes[i].width)
                recomp_fail("ambiguous GLES2 framebuffer width", sizes[i].width);
    }
    s->framebuffer_sizes = sizes;
    s->framebuffer_size_count = count;
}

static Surface *display_image(RecompRDPGles2 *s) {
    RecompViState state = recomp_vi_state(s->rt);
    for (unsigned i = 0; i < 8; i++)
        if (!s->ram_scanout && !state.black && s->present_address && s->surfaces[i].valid &&
            s->surfaces[i].address == s->present_address)
            return &s->surfaces[i];
    /* Loading screens and software video may be drawn by the CPU, without an
     * RSP color surface. Use the OS VI format and a profile-bounded extent. */
    RecompViRegisters vi = {0};
    bool visible = !state.black && s->present_address && recomp_vi_registers(s->rt, &vi) && (vi.status & 3) >= 2;
    unsigned width = s->width, height = s->height;
    if (visible) {
        height = 0;
        width = vi.width;
        for (size_t i = 0; i < s->framebuffer_size_count; i++)
            if (s->framebuffer_sizes[i].width == width)
                height = s->framebuffer_sizes[i].height;
        /* Software-only modes have no RDP color image to establish height.
         * VI vertical start/end are half-lines and the scale has ten fraction bits. */
        if (!height && s->ram_scanout) {
            unsigned lines = (vi.v_start & 1023) - ((vi.v_start >> 16) & 1023);
            height = (lines * (vi.y_scale & 4095) + 2047) / 2048;
        }
        if (!width || width > 1024 || !height || height > 1024)
            recomp_fail("GLES2 VI width is not in the game's framebuffer profile", width);
    }
    Surface *image = &s->cpu_image;
    if (image->valid && (image->width != width || image->height != height))
        recomp_gles2_destroy_surface(s, image);
    if (!image->valid)
        recomp_gles2_create_surface(s, image, width, height);
    if (!visible) {
        s->gl.BindFramebuffer(GL_FRAMEBUFFER, image->framebuffer);
        s->gl.Disable(GL_SCISSOR_TEST);
        s->gl.ClearColor(0, 0, 0, 1);
        s->gl.Clear(GL_COLOR_BUFFER_BIT);
        return image;
    }
    unsigned bpp = (vi.status & 3) == 3 ? 4 : 2;
    size_t count = (size_t)width * height;
    uint32_t address = recomp_ram_address(s->rt, 0x80000000u | s->present_address, count * bpp);
    uint8_t *pixels = malloc(count * 4);
    if (!pixels)
        recomp_fail("GLES2 CPU framebuffer allocation failed", address);
    for (unsigned y = 0; y < height; y++)
        for (unsigned x = 0; x < width; x++) {
            uint32_t src = address + (y * width + x) * bpp;
            uint8_t *dst = pixels + ((size_t)(height - y - 1) * width + x) * 4;
            if (bpp == 2)
                pixel16(load(s, src, 2), dst, false);
            else
                for (unsigned i = 0; i < 4; i++)
                    dst[i] = (uint8_t)load(s, src + i, 1);
        }
    s->gl.ActiveTexture(GL_TEXTURE0);
    s->gl.BindTexture(GL_TEXTURE_2D, image->texture);
    s->gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)width, (GLsizei)height, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    s->textures_dirty = true;
    free(pixels);
    return image;
}
void recomp_gles2_drawable_size(RecompRDPGles2 *s, int *width, int *height) {
#ifdef __EMSCRIPTEN__
    SDL_GetWindowSize(s->window, width, height);
#else
    SDL_GL_GetDrawableSize(s->window, width, height);
#endif
}
static void fit_aspect(int width, int height, double aspect, int *w, int *h) {
    *w = width;
    *h = height;
    if (width < height * aspect)
        *h = (int)(width / aspect + 0.5);
    else
        *w = (int)(height * aspect + 0.5);
}
void recomp_rdp_gles2_present(RecompRDPGles2 *s) {
    recomp_gles2_require_owner(s);
    recomp_gles2_flush(s);
    Surface *image = display_image(s);
    s->gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    int width, height, w, h;
    recomp_gles2_drawable_size(s, &width, &height);
    s->gl.Disable(GL_SCISSOR_TEST);
    s->gl.ClearColor(0, 0, 0, 1);
    s->gl.Clear(GL_COLOR_BUFFER_BIT);
    /* Keep the aspect accepted by the guest camera, not the latest window
     * size: the window can resize after input but before drawing. CPU-only
     * frames never used that projection and must keep their native framing. */
    const double aspect =
        image != &s->cpu_image && s->presentation_aspect > 4.0f / 3 ? s->presentation_aspect : 4.0 / 3;
    if (width > 0 && height > 0) {
        fit_aspect(width, height, aspect, &w, &h);
        if (w > 0 && h > 0)
            blit_texture(s, image, 0, (width - w) / 2, (height - h) / 2, w, h, true);
    }
#ifdef RECOMP_GLES2_CHEATS
    if (s->cheats)
        gles2_cheats_draw(s->cheats);
#endif
#ifdef __EMSCRIPTEN__
    if (emscripten_webgl_commit_frame() != EMSCRIPTEN_RESULT_SUCCESS)
        recomp_fail("WebGL frame presentation failed", 0);
#else
    SDL_GL_SwapWindow(s->window);
#endif
    recomp_gles2_bind_surface(s);
    s->textures_dirty = true;
}

bool recomp_rdp_gles2_capture(RecompRDPGles2 *s, const char *path) {
    recomp_gles2_require_owner(s);
    recomp_gles2_flush(s);
    Surface *f = display_image(s);
    unsigned width = f->width, height = f->height;
    if (s->color_gamma != 1.0f || s->antialias) {
        if (s->capture.valid && (s->capture.width != width || s->capture.height != height))
            recomp_gles2_destroy_surface(s, &s->capture);
        if (!s->capture.valid)
            recomp_gles2_create_surface(s, &s->capture, width, height);
        blit_texture(s, f, s->capture.framebuffer, 0, 0, (int)width, (int)height, true);
    } else
        s->gl.BindFramebuffer(GL_FRAMEBUFFER, f->framebuffer);
    uint8_t *pixels = malloc((size_t)width * height * 4);
    if (!pixels)
        return false;
    s->gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
    s->gl.ReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    bool ok = s->gl.GetError() == GL_NO_ERROR;
    FILE *file = ok ? fopen(path, "wb") : NULL;
    if (!file)
        ok = false;
    else {
        if (fprintf(file, "P6\n%u %u\n255\n", width, height) < 0)
            ok = false;
        for (unsigned y = height; y-- > 0;)
            for (unsigned x = 0; x < width; x++)
                if (fwrite(pixels + ((size_t)y * width + x) * 4, 1, 3, file) != 3)
                    ok = false;
        if (fclose(file))
            ok = false;
    }
    free(pixels);
    recomp_gles2_bind_surface(s);
    return ok;
}
