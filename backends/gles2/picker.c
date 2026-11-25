#include "internal.h"
#include <stdlib.h>

/* One RGBA8 attachment per drawable color image. RGB identifies a visible
 * triangle, A supplies binary coverage to the draw's existing blend state.
 * Contiguous pages share a pair of guest tile keys, including when several
 * guest tiles share a single replacement upload. Zero is the background. */
enum { page_size = 256, max_pages = 65536, cursors = 512 };
typedef struct PickMaterial {
    uint64_t key[2];
    unsigned count;
} PickMaterial;
typedef struct PickCursor {
    unsigned next, end;
} PickCursor;
typedef struct Gles2Picker {
    PickMaterial *pages;
    unsigned count, capacity;
    PickCursor cursor[cursors];
    GLuint outline;
    GLint hover;
} Gles2Picker;

void recomp_gles2_pick_clear(RecompRDPGles2 *s, const Surface *f) {
    if (f->pick_texture) {
        const GLfloat empty[4] = {0};
        s->gl.ClearBufferfv(GL_COLOR, 2, empty);
    }
}

void recomp_gles2_pick_attach(RecompRDPGles2 *s, Surface *f) {
    if (f->pick_texture)
        return;
    s->gl.GenTextures(1, &f->pick_texture);
    s->gl.BindTexture(GL_TEXTURE_2D, f->pick_texture);
    s->gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8_OES, f->width, f->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    s->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    s->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    s->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    s->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    s->gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2_EXT, GL_TEXTURE_2D, f->pick_texture, 0);
    const GLenum targets[] = {GL_COLOR_ATTACHMENT0, f->scene_depth ? GL_COLOR_ATTACHMENT1_EXT : GL_NONE,
                              GL_COLOR_ATTACHMENT2_EXT};
    s->gl.DrawBuffers(3, targets);
    if (s->gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        recomp_fail("GLES triangle picking framebuffer incomplete", f->address);
    s->gl.Disable(GL_SCISSOR_TEST);
    recomp_gles2_pick_clear(s, f);
    s->textures_dirty = true;
}

static void reset_pages(RecompRDPGles2 *s) {
    /* Never let pixels left by partial redraws resolve to a recycled ID. */
    s->gl.Disable(GL_SCISSOR_TEST);
    for (unsigned i = 0; i < 8; ++i) {
        Surface *images[] = {&s->surfaces[i], &s->midpoints[i]};
        for (unsigned k = 0; k < 2; ++k) {
            if (images[k]->pick_texture) {
                s->gl.BindFramebuffer(GL_FRAMEBUFFER, images[k]->framebuffer);
                recomp_gles2_pick_clear(s, images[k]);
            }
        }
    }
    s->gl.BindFramebuffer(GL_FRAMEBUFFER, s->surface->framebuffer);
    memset(s->picker->cursor, 0, sizeof(s->picker->cursor));
    s->picker->count = 1;
}

void recomp_gles2_pick_batch(RecompRDPGles2 *s, const Program *program) {
    if (!s->picker) {
        s->picker = calloc(1, sizeof(*s->picker));
        if (!s->picker)
            recomp_fail("GLES triangle picker allocation failed", 0);
        s->picker->count = 1;
    }
    Gles2Picker *p = s->picker;
    PickMaterial m = {0};
    for (unsigned unit = 0; unit < 2; ++unit)
        if (s->texture_enabled && program->texture[unit] && s->active_texture[unit] != s->white)
            m.key[m.count++] = s->active_tile_key[unit];
    uint64_t hash = m.key[0] ^ (m.key[1] << 1 | m.key[1] >> 63);
    PickCursor *c = &p->cursor[(hash ^ (hash >> 32)) % cursors];
    unsigned triangles = s->batch_count / 3;
    const PickMaterial *old = c->end ? &p->pages[(c->end - 1) / page_size] : NULL;
    if (c->end - c->next < triangles || !old || old->count != m.count || old->key[0] != m.key[0] ||
        old->key[1] != m.key[1]) {
        unsigned pages = (triangles + page_size - 1) / page_size;
        if (p->count + pages > max_pages)
            reset_pages(s);
        unsigned end = p->count + pages;
        if (end > p->capacity) {
            unsigned capacity = SDL_max(end, SDL_max(64u, p->capacity * 2));
            PickMaterial *entries = realloc(p->pages, capacity * sizeof(*entries));
            if (!entries)
                recomp_fail("GLES triangle picker page allocation failed", 0);
            p->pages = entries;
            p->capacity = capacity;
        }
        c->next = p->count * page_size;
        c->end = end * page_size;
        while (p->count < end)
            p->pages[p->count++] = m;
    }
    s->gl.Uniform1i(program->pick_base, (GLint)c->next);
    c->next += triangles;
}

bool recomp_gles2_pick_key(RecompRDPGles2 *s, unsigned id, bool secondary, uint64_t *key) {
    unsigned page = id / page_size;
    if (!page || !s->picker || page >= s->picker->count)
        return false;
    const PickMaterial *m = &s->picker->pages[page];
    if (!m->count)
        return false;
    *key = m->key[secondary && m->count == 2];
    return true;
}

static void init_outline(RecompRDPGles2 *s) {
    const char *vs = "#version 300 es\nin vec4 position;in vec2 uv;out vec2 tc;"
                     "void main(){gl_Position=position;tc=uv;}";
    const char *fs = "#version 300 es\nprecision highp float;precision highp int;"
                     "uniform highp sampler2D ids;uniform int hover;in vec2 tc;out vec4 ink;"
                     "int at(vec2 p){ivec3 b=ivec3(texture(ids,p).rgb*255.0+0.5);return b.r+(b.g<<8)+(b.b<<16);}"
                     "void main(){vec2 stepUV=fwidth(tc);int id=at(tc);"
                     "bool hot=id!=0 && id==hover;vec2 d=stepUV*(hot?2.5:1.0);"
                     "if(id==0 || (at(tc+vec2(d.x,0))==id && at(tc-vec2(d.x,0))==id && "
                     "at(tc+vec2(0,d.y))==id && at(tc-vec2(0,d.y))==id))discard;"
                     "ink=hot?vec4(1.0,0.72,0.08,1.0):vec4(0.15,0.85,0.95,0.72);}";
    GLuint v = recomp_gles2_shader(s, GL_VERTEX_SHADER, vs), f = recomp_gles2_shader(s, GL_FRAGMENT_SHADER, fs);
    GLuint program = s->gl.CreateProgram();
    s->gl.AttachShader(program, v);
    s->gl.AttachShader(program, f);
    s->gl.BindAttribLocation(program, 0, "position");
    s->gl.BindAttribLocation(program, 2, "uv");
    s->gl.LinkProgram(program);
    s->gl.DeleteShader(v);
    s->gl.DeleteShader(f);
    GLint ok;
    s->gl.GetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok)
        recomp_fail("GLES triangle outline shader link failed", 0);
    s->picker->outline = program;
    s->picker->hover = s->gl.GetUniformLocation(program, "hover");
    s->gl.UseProgram(program);
    s->gl.Uniform1i(s->gl.GetUniformLocation(program, "ids"), 0);
}

/* The scene blit leaves its fitted viewport and fullscreen quad bound. */
void recomp_gles2_pick_outline(RecompRDPGles2 *s, const Surface *f, unsigned hovered) {
    if (!f->pick_texture || !s->picker || s->wireframe)
        return;
    if (!s->picker->outline)
        init_outline(s);
    s->gl.UseProgram(s->picker->outline);
    s->gl.Uniform1i(s->picker->hover, (GLint)hovered);
    s->gl.ActiveTexture(GL_TEXTURE0);
    s->gl.BindTexture(GL_TEXTURE_2D, f->pick_texture);
    s->gl.Enable(GL_BLEND);
    s->gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    s->gl.DrawArrays(GL_TRIANGLES, 0, 6);
    s->gl.Disable(GL_BLEND);
}

void recomp_gles2_pick_destroy(RecompRDPGles2 *s) {
    if (s->picker) {
        if (s->picker->outline)
            s->gl.DeleteProgram(s->picker->outline);
        free(s->picker->pages);
        free(s->picker);
    }
}
