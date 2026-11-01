#include "fast3d_gles2.h"
#include "input.h"
#include <SDL.h>
#include <SDL_opengles2.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define GL_FUNCTIONS(X)                                                                                                \
    X(CreateShader, PFNGLCREATESHADERPROC)                                                                             \
    X(ShaderSource, PFNGLSHADERSOURCEPROC)                                                                             \
    X(CompileShader, PFNGLCOMPILESHADERPROC)                                                                           \
    X(GetShaderiv, PFNGLGETSHADERIVPROC)                                                                               \
    X(GetShaderInfoLog, PFNGLGETSHADERINFOLOGPROC)                                                                     \
    X(DeleteShader, PFNGLDELETESHADERPROC)                                                                             \
    X(CreateProgram, PFNGLCREATEPROGRAMPROC)                                                                           \
    X(AttachShader, PFNGLATTACHSHADERPROC)                                                                             \
    X(BindAttribLocation, PFNGLBINDATTRIBLOCATIONPROC)                                                                 \
    X(LinkProgram, PFNGLLINKPROGRAMPROC)                                                                               \
    X(GetProgramiv, PFNGLGETPROGRAMIVPROC)                                                                             \
    X(GetProgramInfoLog, PFNGLGETPROGRAMINFOLOGPROC)                                                                   \
    X(UseProgram, PFNGLUSEPROGRAMPROC)                                                                                 \
    X(DeleteProgram, PFNGLDELETEPROGRAMPROC)                                                                           \
    X(GetUniformLocation, PFNGLGETUNIFORMLOCATIONPROC)                                                                 \
    X(Uniform1i, PFNGLUNIFORM1IPROC)                                                                                   \
    X(Uniform1f, PFNGLUNIFORM1FPROC)                                                                                   \
    X(Uniform4fv, PFNGLUNIFORM4FVPROC)                                                                                 \
    X(GenBuffers, PFNGLGENBUFFERSPROC)                                                                                 \
    X(BindBuffer, PFNGLBINDBUFFERPROC)                                                                                 \
    X(BufferData, PFNGLBUFFERDATAPROC)                                                                                 \
    X(DeleteBuffers, PFNGLDELETEBUFFERSPROC)                                                                           \
    X(EnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAYPROC)                                                       \
    X(VertexAttribPointer, PFNGLVERTEXATTRIBPOINTERPROC)                                                               \
    X(DrawArrays, PFNGLDRAWARRAYSPROC)                                                                                 \
    X(Viewport, PFNGLVIEWPORTPROC)                                                                                     \
    X(Scissor, PFNGLSCISSORPROC)                                                                                       \
    X(ClearColor, PFNGLCLEARCOLORPROC)                                                                                 \
    X(Clear, PFNGLCLEARPROC)                                                                                           \
    X(Enable, PFNGLENABLEPROC)                                                                                         \
    X(Disable, PFNGLDISABLEPROC)                                                                                       \
    X(CullFace, PFNGLCULLFACEPROC)                                                                                     \
    X(DepthFunc, PFNGLDEPTHFUNCPROC)                                                                                   \
    X(DepthMask, PFNGLDEPTHMASKPROC)                                                                                   \
    X(BlendFunc, PFNGLBLENDFUNCPROC)                                                                                   \
    X(PolygonOffset, PFNGLPOLYGONOFFSETPROC)                                                                           \
    X(GetError, PFNGLGETERRORPROC)                                                                                     \
    X(ReadPixels, PFNGLREADPIXELSPROC)                                                                                 \
    X(PixelStorei, PFNGLPIXELSTOREIPROC)                                                                               \
    X(GenTextures, PFNGLGENTEXTURESPROC)                                                                               \
    X(BindTexture, PFNGLBINDTEXTUREPROC)                                                                               \
    X(TexImage2D, PFNGLTEXIMAGE2DPROC)                                                                                 \
    X(TexParameteri, PFNGLTEXPARAMETERIPROC)                                                                           \
    X(DeleteTextures, PFNGLDELETETEXTURESPROC)                                                                         \
    X(ActiveTexture, PFNGLACTIVETEXTUREPROC)                                                                           \
    X(GenFramebuffers, PFNGLGENFRAMEBUFFERSPROC)                                                                       \
    X(BindFramebuffer, PFNGLBINDFRAMEBUFFERPROC)                                                                       \
    X(FramebufferTexture2D, PFNGLFRAMEBUFFERTEXTURE2DPROC)                                                             \
    X(CheckFramebufferStatus, PFNGLCHECKFRAMEBUFFERSTATUSPROC)                                                         \
    X(DeleteFramebuffers, PFNGLDELETEFRAMEBUFFERSPROC)                                                                 \
    X(GenRenderbuffers, PFNGLGENRENDERBUFFERSPROC)                                                                     \
    X(BindRenderbuffer, PFNGLBINDRENDERBUFFERPROC)                                                                     \
    X(RenderbufferStorage, PFNGLRENDERBUFFERSTORAGEPROC)                                                               \
    X(FramebufferRenderbuffer, PFNGLFRAMEBUFFERRENDERBUFFERPROC)                                                       \
    X(DeleteRenderbuffers, PFNGLDELETERENDERBUFFERSPROC)                                                               \
    X(Finish, PFNGLFINISHPROC)

typedef struct Vertex {
    float position[4], color[4], uv[2], fog;
} Vertex;
typedef struct Tile {
    unsigned fmt, siz, line, tmem, palette, cs, ct, ms, mt, ss, st, sl, tl, sh, th;
} Tile;
typedef struct Program {
    uint32_t mux0, mux1, cycle;
    GLuint id;
    GLint prim, env, fog, lod, alpha, fogen;
    GLint tile[2][3];
    bool texture[2];
} Program;
typedef struct Texture {
    uint64_t hash, used;
    GLuint id;
} Texture;
typedef struct Surface {
    uint32_t address;
    GLuint framebuffer, texture, depth;
    bool valid;
    uint64_t frame;
    bool midpoint;
} Surface;
#define HISTORY_SIZE 16384
typedef struct VertexHistory {
    float position[4], local[3];
} VertexHistory;
typedef struct IdentityHistory {
    uint64_t id, frame;
    float position[4], midpoint[4];
    bool matched;
} IdentityHistory;
struct RecompFast3DGles2 {
    Runtime *rt;
    SDL_Window *window;
    SDL_GLContext context;
    SDL_threadID owner;
    RecompInput *input;
    struct {
#define DECLARE(name, type) type name;
        GL_FUNCTIONS(DECLARE)
#undef DECLARE
    } gl;
    RecompFast3D frontend;
    GLuint buffer, white, blit;
    GLint blit_gamma;
    Program programs[256];
    unsigned program_count;
    Texture textures[128];
    uint64_t texture_clock;
    GLuint active_texture[2];
    unsigned tex_width[2], tex_height[2];
    bool textures_dirty;
    Surface surfaces[8];
    Surface midpoints[8];
    Surface readback, capture;
    Surface *surface;
    unsigned render_scale;
    float color_gamma;
    uint32_t color_address, depth_address, present_address;
    bool clearing_depth, depth_clear;
    float projection[4][4], modelview[32][4][4];
    unsigned matrix_depth;
    Vertex vertices[16], batch[1536];
    Vertex midpoint_vertices[16], midpoint_batch[1536];
    VertexHistory *history[2];
    RecompVertexIdentity vertex_identity;
    IdentityHistory *identity_history;
    bool identity_frame, identity_seen;
    unsigned history_count, previous_count;
    uint64_t topology, previous_topology;
    uint64_t frame, presented_frame;
    bool interpolate, recording, history_valid, midpoint_valid, midpoint_changed, history_full;
    uint32_t frame_color;
    bool valid_vertices[16];
    unsigned batch_count;
    uint32_t geometry, other_high, other_low, mux0, mux1, fill_color, blend_color;
    float prim[4], env[4], fog_color[4], prim_lod, texture_s, texture_t;
    unsigned texture_tile;
    bool texture_enabled;
    float lights[9][3], directions[8][3], lookat[2][3];
    unsigned light_count;
    int16_t fog_mul, fog_offset;
    Tile tiles[8];
    uint8_t tmem[4096];
    unsigned image_fmt, image_siz, image_width;
    uint32_t image_address;
    float viewport[4];
    unsigned scissor[4];
    uint32_t half1, rect0, rect1;
    unsigned rect_stage;
    bool rect_flip, full_sync;
    uint64_t triangles;
};
static void flush(RecompFast3DGles2 *s);
static void owner(RecompFast3DGles2 *s) {
    if (SDL_ThreadID() != s->owner || SDL_GL_GetCurrentContext() != s->context)
        recomp_fail("GLES2 requires its owner thread/context", 0);
}
static float clampf(float x, float lo, float hi) {
    return x < lo ? lo : x > hi ? hi : x;
}
static uint64_t hash_word(uint64_t hash, uint64_t word) {
    return (hash ^ word) * UINT64_C(1099511628211);
}
static void identity(float m[4][4]) {
    memset(m, 0, 64);
    for (unsigned i = 0; i < 4; i++)
        m[i][i] = 1;
}
static void transform(const float *v, float m[4][4], float *o) {
    for (unsigned j = 0; j < 4; j++) {
        o[j] = 0;
        for (unsigned i = 0; i < 4; i++)
            o[j] += v[i] * m[i][j];
    }
}
static uint32_t span(RecompFast3DGles2 *s, uint32_t a, size_t n) {
    return recomp_fast3d_address(s->rt, &s->frontend, a, n);
}
static uint32_t load(RecompFast3DGles2 *s, uint32_t a, unsigned n) {
    /* Callers have already resolved and checked the complete physical span.
     * Graphics RAM is not a CPU/device access; avoid the generic bus decoder
     * for every texture byte and vertex component. */
#if RECOMP_LITTLE_ENDIAN_HOST
    if (n == 1)
        return s->rt->rdram[a ^ 3];
    if (a & 1)
        recomp_fail("unaligned Fast3D halfword", a);
    uint16_t value;
    memcpy(&value, s->rt->rdram + (a ^ 2), sizeof(value));
    return value;
#else
    return n == 1 ? s->rt->rdram[a] : (uint32_t)s->rt->rdram[a] << 8 | s->rt->rdram[a + 1];
#endif
}
static void rgba(uint32_t bits, float *c) {
    for (unsigned i = 0; i < 4; i++)
        c[i] = (float)((bits >> (24 - i * 8)) & 255) / 255;
}
static GLuint shader(RecompFast3DGles2 *s, GLenum type, const char *text) {
    GLuint id = s->gl.CreateShader(type);
    s->gl.ShaderSource(id, 1, &text, NULL);
    s->gl.CompileShader(id);
    GLint ok;
    s->gl.GetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        s->gl.GetShaderInfoLog(id, sizeof(log), NULL, log);
        fprintf(stderr, "%s\n%s\n", log, text);
        recomp_fail("GLES2 shader compilation failed", 0);
    }
    return id;
}
static const char *rgb_source(unsigned item, unsigned slot) {
    static const char *common[] = {"combined.rgb", "t0.rgb",  "t1.rgb",    "prim.rgb",
                                   "shade.rgb",    "env.rgb", "vec3(1.0)", "vec3(noise)"};
    if (slot == 2) {
        static const char *mul[] = {"combined.rgb", "t0.rgb",     "t1.rgb",       "prim.rgb",
                                    "shade.rgb",    "env.rgb",    "vec3(0.0)",    "vec3(combined.a)",
                                    "vec3(t0.a)",   "vec3(t1.a)", "vec3(prim.a)", "vec3(shade.a)",
                                    "vec3(env.a)",  "vec3(lod)",  "vec3(lod)",    "vec3(0.0)"};
        return item < 16 ? mul[item] : "vec3(0.0)";
    }
    if (slot == 3)
        return item == 7 ? "vec3(0.0)" : common[item];
    if (slot == 1 && item >= 6)
        return "vec3(0.0)";
    return item < 8 ? common[item] : "vec3(0.0)";
}
static const char *alpha_source(unsigned item, bool mul) {
    static const char *a[] = {"combined.a", "t0.a", "t1.a", "prim.a", "shade.a", "env.a", "1.0", "0.0"};
    return mul ? (item == 0 || item == 6 ? "lod" : a[item]) : a[item];
}
static void texture_use(Program *p, unsigned item, bool rgb_multiplier) {
    if (item == 1 || item == 2)
        p->texture[item - 1] = true;
    if (rgb_multiplier && (item == 8 || item == 9))
        p->texture[item - 8] = true;
}
static Program *program(RecompFast3DGles2 *s) {
    uint32_t cycle = (s->other_high >> 20) & 3;
    for (unsigned i = 0; i < s->program_count; i++)
        if (s->programs[i].mux0 == s->mux0 && s->programs[i].mux1 == s->mux1 && s->programs[i].cycle == cycle)
            return &s->programs[i];
    if (s->program_count == 256)
        recomp_fail("GLES2 combiner program cache exhausted", s->mux1);
    Program *p = &s->programs[s->program_count++];
    p->mux0 = s->mux0;
    p->mux1 = s->mux1;
    p->cycle = cycle;
    const char *vs = "attribute vec4 position;attribute vec4 color;attribute vec2 uv;attribute float fogv;"
                     "varying vec4 shade;varying vec2 texcoord;varying float fogfactor;"
                     "void main(){gl_Position=position;shade=color;texcoord=uv;fogfactor=fogv;}";
    char fs[8192];
    int pos =
        snprintf(fs, sizeof(fs),
                 "#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n"
                 "varying vec4 shade;varying vec2 texcoord;varying float fogfactor;uniform vec4 prim,env,fogc;uniform "
                 "float lod,alphatest,fogen;"
                 "uniform sampler2D tex0,tex1;uniform vec4 dim0,dim1,origin0,origin1,wrap0,wrap1;"
                 "vec2 coord(vec2 uv,vec4 d,vec4 o,vec4 w){vec2 p=uv*d.zw-o.xy;"
                 "if(w.x>0.0){p.x=mod(p.x,w.x*(w.z>0.0?2.0:1.0));if(w.z>0.0)p.x=min(p.x,w.x*2.0-1.0-p.x);}"
                 "if(w.y>0.0){p.y=mod(p.y,w.y*(w.w>0.0?2.0:1.0));if(w.w>0.0)p.y=min(p.y,w.y*2.0-1.0-p.y);}"
                 "p=clamp(p,vec2(0.0),d.xy-vec2(1.0));return (p+vec2(0.5))/d.xy;}"
                 "void main(){vec4 t0=texture2D(tex0,coord(texcoord,dim0,origin0,wrap0));vec4 "
                 "t1=texture2D(tex1,coord(texcoord,dim1,origin1,wrap1));"
                 "float noise=fract(sin(dot(gl_FragCoord.xy,vec2(12.9898,78.233)))*43758.5);vec4 combined=vec4(0.0);");
    if (cycle == 2) {
        pos += snprintf(fs + pos, sizeof(fs) - (size_t)pos, "combined=t0;");
        p->texture[0] = true;
    }
    for (unsigned c = cycle == 1 ? 0 : 1; c < 2 && cycle != 2; c++) {
        unsigned a = c ? (s->mux0 >> 5) & 15 : (s->mux0 >> 20) & 15, b = c ? (s->mux1 >> 24) & 15 : s->mux1 >> 28;
        unsigned m = c ? s->mux0 & 31 : (s->mux0 >> 15) & 31, d = c ? (s->mux1 >> 6) & 7 : (s->mux1 >> 15) & 7;
        unsigned aa = c ? (s->mux1 >> 21) & 7 : (s->mux0 >> 12) & 7, ab = c ? (s->mux1 >> 3) & 7 : (s->mux1 >> 12) & 7;
        unsigned ac = c ? (s->mux1 >> 18) & 7 : (s->mux0 >> 9) & 7, ad = c ? s->mux1 & 7 : (s->mux1 >> 9) & 7;
        texture_use(p, d, false);
        texture_use(p, ad, false);
        if (m != 31 && a != b) {
            texture_use(p, a, false);
            texture_use(p, b, false);
            texture_use(p, m, true);
        }
        if (ac != 7 && aa != ab) {
            texture_use(p, aa, false);
            texture_use(p, ab, false);
            texture_use(p, ac, false);
        }
        pos +=
            snprintf(fs + pos, sizeof(fs) - (size_t)pos, "combined=clamp(vec4((%s-%s)*%s+%s,(%s-%s)*%s+%s),0.0,1.0);",
                     rgb_source(a, 0), rgb_source(b, 1), rgb_source(m, 2), rgb_source(d, 3), alpha_source(aa, false),
                     alpha_source(ab, false), alpha_source(ac, true), alpha_source(ad, false));
    }
    snprintf(fs + pos, sizeof(fs) - (size_t)pos,
             "if(alphatest>=0.0&&combined.a<alphatest)discard;combined.rgb=mix(combined.rgb,fogc.rgb,clamp(fogfactor*"
             "fogen,0.0,1.0));gl_FragColor=combined;}");
    GLuint v = shader(s, GL_VERTEX_SHADER, vs), f = shader(s, GL_FRAGMENT_SHADER, fs);
    p->id = s->gl.CreateProgram();
    s->gl.AttachShader(p->id, v);
    s->gl.AttachShader(p->id, f);
    const char *attrs[] = {"position", "color", "uv", "fogv"};
    for (unsigned i = 0; i < 4; i++)
        s->gl.BindAttribLocation(p->id, i, attrs[i]);
    s->gl.LinkProgram(p->id);
    s->gl.DeleteShader(v);
    s->gl.DeleteShader(f);
    GLint ok;
    s->gl.GetProgramiv(p->id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        s->gl.GetProgramInfoLog(p->id, sizeof(log), NULL, log);
        fprintf(stderr, "%s\n", log);
        recomp_fail("GLES2 combiner link failed", 0);
    }
    p->prim = s->gl.GetUniformLocation(p->id, "prim");
    p->env = s->gl.GetUniformLocation(p->id, "env");
    p->fog = s->gl.GetUniformLocation(p->id, "fogc");
    p->lod = s->gl.GetUniformLocation(p->id, "lod");
    p->alpha = s->gl.GetUniformLocation(p->id, "alphatest");
    p->fogen = s->gl.GetUniformLocation(p->id, "fogen");
    const char *names[2][3] = {{"dim0", "origin0", "wrap0"}, {"dim1", "origin1", "wrap1"}};
    for (unsigned i = 0; i < 2; i++) {
        for (unsigned j = 0; j < 3; j++)
            p->tile[i][j] = s->gl.GetUniformLocation(p->id, names[i][j]);
    }
    s->gl.UseProgram(p->id);
    s->gl.Uniform1i(s->gl.GetUniformLocation(p->id, "tex0"), 0);
    s->gl.Uniform1i(s->gl.GetUniformLocation(p->id, "tex1"), 1);
    return p;
}
static void create_surface(RecompFast3DGles2 *s, Surface *f, unsigned width, unsigned height, bool depth) {
    f->valid = true;
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
    if (depth) {
        s->gl.GenRenderbuffers(1, &f->depth);
        s->gl.BindRenderbuffer(GL_RENDERBUFFER, f->depth);
        s->gl.RenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, (GLsizei)width, (GLsizei)height);
        s->gl.FramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, f->depth);
    }
    if (s->gl.CheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        recomp_fail("GLES2 framebuffer incomplete", f->address);
    s->gl.Disable(GL_SCISSOR_TEST);
    s->gl.DepthMask(GL_TRUE);
    s->gl.ClearColor(0, 0, 0, 1);
    s->gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    s->textures_dirty = true;
}
static Surface *surface(RecompFast3DGles2 *s, uint32_t address) {
    for (unsigned i = 0; i < 8; i++)
        if (s->surfaces[i].valid && s->surfaces[i].address == address)
            return &s->surfaces[i];
    Surface *f = NULL;
    for (unsigned i = 0; i < 8; i++)
        if (!s->surfaces[i].valid) {
            f = &s->surfaces[i];
            break;
        }
    if (!f)
        recomp_fail("GLES2 framebuffer cache exhausted", address);
    f->address = address;
    create_surface(s, f, 320 * s->render_scale, 240 * s->render_scale, true);
    return f;
}
static void bind_surface(RecompFast3DGles2 *s) {
    s->surface = surface(s, s->color_address);
    Surface *mid = &s->midpoints[s->surface - s->surfaces];
    if (s->recording && !mid->valid)
        create_surface(s, mid, 320 * s->render_scale, 240 * s->render_scale, true);
    s->gl.BindFramebuffer(GL_FRAMEBUFFER, s->surface->framebuffer);
    if (s->depth_clear) {
        s->gl.Disable(GL_SCISSOR_TEST);
        s->gl.DepthMask(GL_TRUE);
        s->gl.Clear(GL_DEPTH_BUFFER_BIT);
        if (s->recording) {
            s->gl.BindFramebuffer(GL_FRAMEBUFFER, mid->framebuffer);
            s->gl.Clear(GL_DEPTH_BUFFER_BIT);
            s->gl.BindFramebuffer(GL_FRAMEBUFFER, s->surface->framebuffer);
        }
        s->depth_clear = false;
    }
}
static unsigned byte(RecompFast3DGles2 *s, unsigned offset) {
    return s->tmem[offset & 4095];
}
static void pixel16(unsigned bits, uint8_t *p, bool ia) {
    if (ia) {
        p[0] = p[1] = p[2] = (uint8_t)(bits >> 8);
        p[3] = (uint8_t)bits;
    } else {
        p[0] = (uint8_t)(((bits >> 11) & 31) * 255 / 31);
        p[1] = (uint8_t)(((bits >> 6) & 31) * 255 / 31);
        p[2] = (uint8_t)(((bits >> 1) & 31) * 255 / 31);
        p[3] = (bits & 1) ? 255 : 0;
    }
}
static GLuint texture(RecompFast3DGles2 *s, Tile *t, unsigned *width, unsigned *height) {
    *width = t->sh >= t->sl ? (t->sh - t->sl) / 4 + 1 : 1;
    *height = t->th >= t->tl ? (t->th - t->tl) / 4 + 1 : 1;
    if (*width > 1024 || *height > 1024 || !t->line) {
        fprintf(stderr, "tile=%u fmt=%u siz=%u line=%u tmem=%u size=%ux%u active=%u mux=%06x:%08x mode=%08x\n",
                (unsigned)(t - s->tiles), t->fmt, t->siz, t->line, t->tmem, *width, *height, s->texture_tile, s->mux0,
                s->mux1, s->other_high);
        recomp_fail("GLES2 unsupported tile dimensions", t->sh);
    }
    uint64_t hash = UINT64_C(1469598103934665603);
    unsigned tlut = (s->other_high >> 14) & 3, mask = (t->siz == 3 || tlut) ? 2047 : 4095;
    unsigned row_bytes = t->siz == 3 ? *width * 2 : (*width * (4u << t->siz) + 7) / 8;
    /* Cache the tile's contents, independent of unrelated TMEM loads and of
     * where the game placed the same texture in TMEM. */
    for (unsigned y = 0; y < *height; y++)
        for (unsigned bank = 0; bank < (t->siz == 3 ? 2u : 1u); bank++)
            for (unsigned x = 0; x < row_bytes; x++) {
                unsigned offset = (t->tmem * 8 + y * t->line * 8 + x) ^ ((y & 1) ? 4 : 0);
                hash ^= byte(s, (offset & mask) | (bank * 2048));
                hash *= UINT64_C(1099511628211);
            }
    if (tlut && t->fmt == 2)
        for (unsigned i = 2048; i < 4096; i++) {
            hash ^= s->tmem[i];
            hash *= UINT64_C(1099511628211);
        }
    unsigned key[] = {*width, *height, t->fmt, t->siz, t->palette, tlut};
    for (unsigned i = 0; i < sizeof(key) / sizeof(*key); i++) {
        hash ^= key[i];
        hash *= UINT64_C(1099511628211);
    }
    Texture *entry = NULL;
    for (unsigned i = 0; i < 128; i++)
        if (s->textures[i].id && s->textures[i].hash == hash) {
            s->textures[i].used = ++s->texture_clock;
            return s->textures[i].id;
        }
    for (unsigned i = 0; i < 128; i++)
        if (!entry || s->textures[i].used < entry->used)
            entry = &s->textures[i];
    if (!entry->id)
        s->gl.GenTextures(1, &entry->id);
    size_t size = (size_t)*width * *height * 4;
    uint8_t *pixels = malloc(size);
    if (!pixels)
        recomp_fail("GLES2 texture allocation failed", 0);
    for (unsigned y = 0; y < *height; y++)
        for (unsigned x = 0; x < *width; x++) {
            unsigned o = t->tmem * 8 + y * t->line * 8 + ((x << (t->siz == 3 ? 2 : t->siz)) >> 1);
            o ^= (y & 1) ? 4 : 0;
            unsigned b0 = byte(s, o & mask), b1 = byte(s, (o + 1) & mask), nibble = (b0 >> ((x & 1) ? 0 : 4)) & 15;
            uint8_t *p = pixels + ((size_t)y * *width + x) * 4;
            if (tlut && t->fmt == 2) {
                unsigned index = t->siz == 0 ? t->palette * 16 + nibble : b0;
                unsigned k = 2048 + index * 8;
                pixel16(byte(s, k) * 256 + byte(s, k + 1), p, tlut == 3);
            } else if (t->siz == 0) {
                unsigned i = nibble * 17, a = i;
                if (t->fmt == 3) {
                    i = ((nibble >> 1) * 255) / 7;
                    a = (nibble & 1) ? 255 : 0;
                }
                p[0] = p[1] = p[2] = (uint8_t)i;
                p[3] = (uint8_t)a;
            } else if (t->siz == 1) {
                unsigned i = b0, a = b0;
                if (t->fmt == 3) {
                    i = (b0 >> 4) * 17;
                    a = (b0 & 15) * 17;
                }
                p[0] = p[1] = p[2] = (uint8_t)i;
                p[3] = (uint8_t)a;
            } else if (t->siz == 2)
                pixel16(b0 * 256 + b1, p, t->fmt == 3);
            else {
                p[0] = (uint8_t)b0;
                p[1] = (uint8_t)b1;
                p[2] = (uint8_t)byte(s, (o & 2047) | 2048);
                p[3] = (uint8_t)byte(s, ((o + 1) & 2047) | 2048);
            }
        }
    s->gl.BindTexture(GL_TEXTURE_2D, entry->id);
    s->gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)*width, (GLsizei)*height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                     pixels);
    free(pixels);
    s->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    s->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    entry->hash = hash;
    entry->used = ++s->texture_clock;
    return entry->id;
}
static void flush(RecompFast3DGles2 *s) {
    if (!s->batch_count)
        return;
    Program *p = program(s);
    s->gl.UseProgram(p->id);
    s->gl.Uniform4fv(p->prim, 1, s->prim);
    s->gl.Uniform4fv(p->env, 1, s->env);
    s->gl.Uniform4fv(p->fog, 1, s->fog_color);
    s->gl.Uniform1f(p->lod, s->prim_lod);
    bool alpha = (s->other_low & 0x1000) || (s->other_low & 3);
    s->gl.Uniform1f(p->alpha, alpha ? (s->other_low & 3 ? ((float)(s->blend_color & 255) / 255) : 0.5f) : -1);
    s->gl.Uniform1f(p->fogen, (s->geometry & 0x10000) ? 1.0f : 0.0f);
    for (unsigned i = 0; i < 2; i++) {
        Tile *t = &s->tiles[(s->texture_tile + i) & 7];
        s->gl.ActiveTexture(GL_TEXTURE0 + i);
        if (!s->texture_enabled || !p->texture[i]) {
            s->active_texture[i] = s->white;
            s->tex_width[i] = s->tex_height[i] = 1;
        } else if (s->textures_dirty)
            s->active_texture[i] = texture(s, t, &s->tex_width[i], &s->tex_height[i]);
        s->gl.BindTexture(GL_TEXTURE_2D, s->active_texture[i]);
        GLint filter = p->cycle == 2 ? GL_NEAREST : GL_LINEAR;
        s->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
        s->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
        float dim[4] = {(float)s->tex_width[i], (float)s->tex_height[i],
                        t->ss > 10 ? (float)(1u << (16 - t->ss)) : 1.0f / (float)(1u << t->ss),
                        t->st > 10 ? (float)(1u << (16 - t->st)) : 1.0f / (float)(1u << t->st)};
        float origin[4] = {(float)t->sl / 4, (float)t->tl / 4, 0, 0};
        float wrap[4] = {t->ms && !(t->cs & 2) ? (float)(1u << t->ms) : 0,
                         t->mt && !(t->ct & 2) ? (float)(1u << t->mt) : 0, (float)(t->cs & 1), (float)(t->ct & 1)};
        s->gl.Uniform4fv(p->tile[i][0], 1, dim);
        s->gl.Uniform4fv(p->tile[i][1], 1, origin);
        s->gl.Uniform4fv(p->tile[i][2], 1, wrap);
    }
    s->textures_dirty = false;
    bool depth = (s->geometry & 1) && (s->other_low & 0x10);
    if (depth)
        s->gl.Enable(GL_DEPTH_TEST);
    else
        s->gl.Disable(GL_DEPTH_TEST);
    s->gl.DepthFunc(GL_LEQUAL);
    s->gl.DepthMask((s->other_low & 0x20) ? GL_TRUE : GL_FALSE);
    if ((s->other_low & 0xc00) == 0xc00) {
        s->gl.Enable(GL_POLYGON_OFFSET_FILL);
        s->gl.PolygonOffset(-1, -1);
    } else
        s->gl.Disable(GL_POLYGON_OFFSET_FILL);
    if (s->other_low & 0x4000) {
        s->gl.Enable(GL_BLEND);
        s->gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else
        s->gl.Disable(GL_BLEND);
    if (s->geometry & 0x3000) {
        s->gl.Enable(GL_CULL_FACE);
        s->gl.CullFace((s->geometry & 0x3000) == 0x3000 ? GL_FRONT_AND_BACK
                       : s->geometry & 0x1000           ? GL_FRONT
                                                        : GL_BACK);
    } else
        s->gl.Disable(GL_CULL_FACE);
    float scale = (float)s->render_scale;
    s->gl.Viewport((GLint)(s->viewport[0] * scale), (GLint)((240 - s->viewport[1] - s->viewport[3]) * scale),
                   (GLsizei)(s->viewport[2] * scale), (GLsizei)(s->viewport[3] * scale));
    s->gl.Enable(GL_SCISSOR_TEST);
    s->gl.Scissor((GLint)(s->scissor[0] * s->render_scale), (GLint)((240 - s->scissor[3]) * s->render_scale),
                  (GLsizei)((s->scissor[2] - s->scissor[0]) * s->render_scale),
                  (GLsizei)((s->scissor[3] - s->scissor[1]) * s->render_scale));
    s->gl.BindBuffer(GL_ARRAY_BUFFER, s->buffer);
    s->gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(s->batch_count * sizeof(Vertex)), s->batch, GL_STREAM_DRAW);
    for (unsigned i = 0; i < 4; i++)
        s->gl.EnableVertexAttribArray(i);
    s->gl.VertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, position));
    s->gl.VertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, color));
    s->gl.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, uv));
    s->gl.VertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void *)offsetof(Vertex, fog));
    s->gl.DrawArrays(GL_TRIANGLES, 0, (GLsizei)s->batch_count);
    if (s->recording) {
        Surface *mid = &s->midpoints[s->surface - s->surfaces];
        s->gl.BindFramebuffer(GL_FRAMEBUFFER, mid->framebuffer);
        s->gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(s->batch_count * sizeof(Vertex)), s->midpoint_batch,
                         GL_STREAM_DRAW);
        s->gl.DrawArrays(GL_TRIANGLES, 0, (GLsizei)s->batch_count);
        s->gl.BindFramebuffer(GL_FRAMEBUFFER, s->surface->framebuffer);
    }
    s->triangles += s->batch_count / 3;
    s->batch_count = 0;
}

static void load_texture(RecompFast3DGles2 *s, Tile *t, uint32_t w0, uint32_t w1, bool block, bool palette) {
    unsigned sl = (w0 >> 12) & 4095, tl = w0 & 4095, sh = (w1 >> 12) & 4095, th = w1 & 4095;
    unsigned bits = 4u << s->image_siz;
    if (palette) {
        unsigned count = ((w1 >> 14) & 1023) + 1;
        uint32_t src = s->image_address + (((tl >> 2) * s->image_width + (sl >> 2)) * 2);
        recomp_ram_address(s->rt, src, count * 2);

        for (unsigned i = 0; i < count; i++)
            for (unsigned lane = 0; lane < 4; lane++) {
                unsigned dst = (t->tmem * 8 + i * 8 + lane * 2) & 4095;
                s->tmem[dst] = (uint8_t)load(s, src + i * 2, 1);
                s->tmem[(dst + 1) & 4095] = (uint8_t)load(s, src + i * 2 + 1, 1);
            }
    } else if (block) {
        unsigned count = sh + 1, bytes = (count * (4u << t->siz) + 7) / 8;
        uint32_t src = s->image_address + (((tl >> 2) * s->image_width + (sl >> 2)) * bits / 8);
        recomp_ram_address(s->rt, src, bytes);

        for (unsigned i = 0; i < bytes; i++) {
            unsigned word = i / 8, row = (word * th) >> 11, offset = t->tmem * 8 + i;
            if (t->siz == 3) {
                unsigned pixel = i / 4, part = i & 3;
                offset = t->tmem * 8 + pixel * 2 + (part & 1);
                offset = (offset & 2047) | (part >= 2 ? 2048 : 0);
            }
            offset ^= (row & 1) ? 4 : 0;
            s->tmem[offset & 4095] = (uint8_t)load(s, src + i, 1);
        }
    } else {
        unsigned width = (sh - sl) / 4 + 1, height = (th - tl) / 4 + 1;
        if (sh < sl || th < tl || width > 1024 || height > 1024)
            recomp_fail("GLES2 malformed texture tile load", w0);
        for (unsigned y = 0; y < height; y++)
            for (unsigned x = 0; x < width; x++) {
                uint64_t bit = ((uint64_t)((tl >> 2) + y) * s->image_width + (sl >> 2) + x) * bits;
                uint32_t src = s->image_address + (uint32_t)(bit / 8);
                unsigned bpp = bits >= 8 ? bits / 8 : 1;
                recomp_ram_address(s->rt, src, bpp);

                for (unsigned b = 0; b < bpp; b++) {
                    unsigned offset = t->tmem * 8 + y * t->line * 8 + ((x * bits) >> 3) + b;
                    if (bits == 32)
                        offset = ((t->tmem * 8 + y * t->line * 8 + x * 2 + (b & 1)) & 2047) | (b >= 2 ? 2048 : 0);
                    offset ^= (y & 1) ? 4 : 0;
                    offset &= 4095;
                    unsigned value = load(s, src + b, 1);
                    if (bits == 4) {
                        unsigned nibble = (value >> ((bit & 4) ? 0 : 4)) & 15, shift = (x & 1) ? 0 : 4;
                        s->tmem[offset] = (uint8_t)((s->tmem[offset] & ~(15u << shift)) | (nibble << shift));
                    } else
                        s->tmem[offset] = (uint8_t)value;
                }
            }
    }
    s->textures_dirty = true;
}
/* Game-provided logical identities survive repacking of the same mesh. A
 * missing previous position or ambiguous transform rejects the whole pose. */
static void interpolate_identity(RecompFast3DGles2 *s, uint64_t id, const Vertex *current, Vertex *mid) {
    s->identity_seen = true;
    unsigned hash = (unsigned)hash_word(id >> 32, id) & (HISTORY_SIZE - 1);
    IdentityHistory *entry = NULL, *reusable = NULL;
    for (unsigned i = 0; i < HISTORY_SIZE; i++) {
        IdentityHistory *candidate = &s->identity_history[(hash + i) & (HISTORY_SIZE - 1)];
        if (candidate->id == id || !candidate->id) {
            entry = candidate;
            break;
        }
        if (!reusable && candidate->frame + 1 < s->frame)
            reusable = candidate;
    }
    if (!entry)
        entry = reusable;
    if (!entry) {
        s->midpoint_valid = false;
        return;
    }
    if (entry->id != id || entry->frame != s->frame) {
        bool matched = entry->id == id && entry->frame + 1 == s->frame;
        float extent = fmaxf(128, fmaxf(fabsf(entry->position[3]), fabsf(current->position[3])));
        for (unsigned j = 0; j < 4; j++) {
            float p = entry->position[j], q = current->position[j];
            if (!isfinite(p) || !isfinite(q) || fabsf(p - q) > extent * 0.5f)
                matched = false;
            entry->midpoint[j] = (p + q) * 0.5f;
        }
        entry->id = id;
        entry->frame = s->frame;
        entry->matched = matched;
        memcpy(entry->position, current->position, sizeof(entry->position));
    } else if (memcmp(entry->position, current->position, sizeof(entry->position))) {
        /* One identity under different transforms is ambiguous. Reject the
         * complete candidate, including copies already submitted to the GPU. */
        entry->matched = false;
    }
    if (!entry->matched) {
        s->midpoint_valid = false;
        return;
    }
    memcpy(mid->position, entry->midpoint, sizeof(mid->position));
    if (memcmp(mid->position, current->position, sizeof(mid->position)))
        s->midpoint_changed = true;
    mid->fog = mid->position[3] > 0
                   ? clampf((mid->position[2] / mid->position[3] * s->fog_mul + s->fog_offset) / 255, 0, 1)
                   : 1;
}
/* Without explicit identities, pair vertex loads only when the complete
 * command topology matches. Colors/normals are payload, not identity. */
static void interpolate_vertex(RecompFast3DGles2 *s, unsigned index, uint32_t address, const float local[4]) {
    Vertex *current = &s->vertices[index], *mid = &s->midpoint_vertices[index];
    *mid = *current;
    unsigned slot = s->history_count++;
    if (slot >= HISTORY_SIZE) {
        s->midpoint_valid = false;
        s->history_full = true;
        return;
    }
    VertexHistory *entry = &s->history[1][slot];
    memcpy(entry->position, current->position, sizeof(entry->position));
    memcpy(entry->local, local, sizeof(entry->local));
    if (s->identity_frame) {
        uint64_t id = s->vertex_identity.identify(s->vertex_identity.userdata, address, local);
        if (id)
            interpolate_identity(s, id, current, mid);
        return;
    }
    if (!s->history_valid || slot >= s->previous_count) {
        s->midpoint_valid = false;
        return;
    }
    const VertexHistory *previous = &s->history[0][slot];
    const float *p = previous->position, *q = current->position;
    /* Orthographic UI remains native. Perspective vertices are interpolated in
     * homogeneous coordinates, including clipped/behind-camera vertices, so
     * adjacent triangles do not separate at the near plane. */
    if (p[3] == 1 && q[3] == 1)
        return;
    float extent = fmaxf(128, fmaxf(fabsf(p[3]), fabsf(q[3])));
    for (unsigned j = 0; j < 4; j++) {
        if (!isfinite(p[j]) || !isfinite(q[j]) || fabsf(p[j] - q[j]) > extent * 0.5f ||
            (j < 3 && fabsf(previous->local[j] - local[j]) > 128)) {
            s->midpoint_valid = false;
            return;
        }
    }
    for (unsigned j = 0; j < 4; j++) {
        mid->position[j] = (p[j] + q[j]) * 0.5f;
        if (mid->position[j] != q[j])
            s->midpoint_changed = true;
    }
    mid->fog = mid->position[3] > 0
                   ? clampf((mid->position[2] / mid->position[3] * s->fog_mul + s->fog_offset) / 255, 0, 1)
                   : 1;
}
static void triangle(RecompFast3DGles2 *s, unsigned a, unsigned b, unsigned c) {
    if (a >= 16 || b >= 16 || c >= 16 || !s->valid_vertices[a] || !s->valid_vertices[b] || !s->valid_vertices[c])
        recomp_fail("GLES2 triangle references an unloaded vertex", a);
    if (s->batch_count + 3 > 1536)
        flush(s);
    const unsigned indices[] = {a, b, c};
    for (unsigned i = 0; i < 3; i++) {
        unsigned index = indices[i];
        s->batch[s->batch_count] = s->vertices[index];
        if (s->recording)
            s->midpoint_batch[s->batch_count] = s->midpoint_vertices[index];
        s->batch_count++;
    }
}
static void rectangle(RecompFast3DGles2 *s, float x0, float y0, float x1, float y1, float u, float v, float du,
                      float dv, bool flip) {
    flush(s);
    float oldviewport[4];
    memcpy(oldviewport, s->viewport, sizeof(oldviewport));
    s->viewport[0] = s->viewport[1] = 0;
    s->viewport[2] = 320;
    s->viewport[3] = 240;
    uint32_t geometry = s->geometry;
    s->geometry = 0;
    Vertex q[4] = {0};
    float x[4] = {x0, x1, x1, x0}, y[4] = {y0, y0, y1, y1};
    for (unsigned i = 0; i < 4; i++) {
        q[i].position[0] = x[i] / 160 - 1;
        q[i].position[1] = 1 - y[i] / 120;
        q[i].position[3] = 1;
        for (unsigned j = 0; j < 4; j++)
            q[i].color[j] = 1;
        q[i].uv[0] = u + (flip ? y[i] - y0 : x[i] - x0) * du;
        q[i].uv[1] = v + (flip ? x[i] - x0 : y[i] - y0) * dv;
    }
    const unsigned order[] = {0, 1, 2, 0, 2, 3};
    for (unsigned i = 0; i < 6; i++)
        s->batch[s->batch_count++] = q[order[i]];
    if (s->recording)
        memcpy(s->midpoint_batch, s->batch, s->batch_count * sizeof(Vertex));
    flush(s);
    s->geometry = geometry;
    memcpy(s->viewport, oldviewport, sizeof(oldviewport));
}
static void vertex(RecompFast3DGles2 *s, uint32_t address, unsigned first, unsigned count) {
    if (count > 16 - first)
        recomp_fail("GLES2 vertex cache overflow", address);
    address = span(s, address, count * 16);
    for (unsigned i = 0; i < count; i++) {
        uint32_t a = address + i * 16;
        float local[4], eye[4];
        Vertex *v = &s->vertices[first + i];
        for (unsigned j = 0; j < 3; j++)
            local[j] = (float)(int16_t)load(s, a + j * 2, 2);
        local[3] = 1;
        transform(local, s->modelview[s->matrix_depth], eye);
        transform(eye, s->projection, v->position);
        for (unsigned j = 0; j < 4; j++)
            v->color[j] = (float)load(s, a + 12 + j, 1) / 255;
        v->uv[0] = (float)(int16_t)load(s, a + 8, 2) / 32 * s->texture_s;
        v->uv[1] = (float)(int16_t)load(s, a + 10, 2) / 32 * s->texture_t;
        if (s->geometry & 0x20000) {
            float n[3], normal[3] = {0};
            for (unsigned j = 0; j < 3; j++)
                n[j] = (float)(int8_t)load(s, a + 12 + j, 1) / 127;
            for (unsigned j = 0; j < 3; j++)
                for (unsigned k = 0; k < 3; k++)
                    normal[j] += n[k] * s->modelview[s->matrix_depth][k][j];
            float length = sqrtf(normal[0] * normal[0] + normal[1] * normal[1] + normal[2] * normal[2]);
            if (length > 0)
                for (unsigned j = 0; j < 3; j++)
                    normal[j] /= length;
            for (unsigned j = 0; j < 3; j++)
                v->color[j] = s->lights[s->light_count][j];
            for (unsigned l = 0; l < s->light_count; l++) {
                float dot = 0;
                for (unsigned j = 0; j < 3; j++)
                    dot += normal[j] * s->directions[l][j];
                if (dot > 0)
                    for (unsigned j = 0; j < 3; j++)
                        v->color[j] += dot * s->lights[l][j];
            }
            for (unsigned j = 0; j < 3; j++)
                v->color[j] = clampf(v->color[j], 0, 1);
            if (s->geometry & 0x40000)
                for (unsigned j = 0; j < 2; j++) {
                    float dot = 0;
                    for (unsigned k = 0; k < 3; k++)
                        dot += normal[k] * s->lookat[j][k];
                    dot = clampf(dot, -1, 1);
                    v->uv[j] = (s->geometry & 0x80000 ? acosf(dot) * 325.949323f : (dot + 1) * 512) *
                               (j ? s->texture_t : s->texture_s);
                }
        }
        v->fog =
            v->position[3] > 0 ? clampf((v->position[2] / v->position[3] * s->fog_mul + s->fog_offset) / 255, 0, 1) : 1;
        if (s->recording)
            interpolate_vertex(s, first + i, a, local);
        s->valid_vertices[first + i] = true;
    }
}
static bool consume(Runtime *rt, void *opaque, const RecompFast3D *state, const RecompFast3DCommand *cmd) {
    RecompFast3DGles2 *s = opaque;

    unsigned op = cmd->w0 >> 24;
    uint32_t w0 = cmd->w0, w1 = cmd->w1;
    (void)rt;
    (void)state;
    if (s->recording) {
        /* Task-buffer addresses, textures and transforms are allowed to change;
         * command order, vertex slots and triangle connectivity must match. */
        s->topology = hash_word(s->topology, w0);
        if (op == 0xbf || op == 0xb6 || op == 0xb7 || op == 0xbd || op == 0xfc)
            s->topology = hash_word(s->topology, w1);
    }
    /* Vertices are transformed/lit when loaded and copied into the batch.
     * Matrix/vertex updates and list control do not change its raster state. */
    if (op != 0xbf && op != 0x01 && op != 0x04 && op != 0x06 && op != 0xb8 && op != 0xbc && op != 0xbd && op != 0x00 &&
        op != 0xc0 && !(op == 0x03 && ((w0 >> 16) & 255) != 0x80))
        flush(s);
    switch (op) {
        case 0x00:
        case 0x06:
        case 0xb8:
        case 0xc0:
        case 0xe6:
        case 0xe7:
        case 0xe8:
            return true;
        case 0xe9:
            s->full_sync = true;
            return true;
        case 0x01: {
            unsigned flags = (w0 >> 16) & 255;
            if (flags & ~7u || ((flags & 1) && (flags & 4)))
                return false;
            uint32_t a = span(s, w1, 64);
            float matrix[4][4];
            for (unsigned i = 0; i < 16; i++)
                matrix[i / 4][i % 4] =
                    (float)((double)(int16_t)load(s, a + i * 2, 2) + (double)load(s, a + 32 + i * 2, 2) / 65536);
            if (flags & 4) {
                if (s->matrix_depth == 31)
                    recomp_fail("GLES2 matrix stack exhausted", cmd->address);
                memcpy(s->modelview[s->matrix_depth + 1], s->modelview[s->matrix_depth], 64);

                s->matrix_depth++;
            }
            float (*target)[4] = flags & 1 ? s->projection : s->modelview[s->matrix_depth];
            if (flags & 2)
                memcpy(target, matrix, 64);
            else {
                float result[4][4];
                for (unsigned i = 0; i < 4; i++)
                    transform(matrix[i], target, result[i]);
                memcpy(target, result, 64);
            }
            return true;
        }
        case 0xbd:
            if (w1 || !s->matrix_depth)
                return false;
            s->matrix_depth--;
            return true;
        case 0x04:
            vertex(s, w1, (w0 >> 16) & 15, ((w0 >> 20) & 15) + 1);
            return true;
        case 0x03: {
            unsigned index = (w0 >> 16) & 255;
            uint32_t a = span(s, w1, index == 0x80 ? 16 : 24);
            if (index == 0x80) {
                float sx = (float)(int16_t)load(s, a, 2) / 4, sy = (float)(int16_t)load(s, a + 2, 2) / 4;
                s->viewport[0] = (float)(int16_t)load(s, a + 8, 2) / 4 - sx;
                s->viewport[1] = (float)(int16_t)load(s, a + 10, 2) / 4 - sy;
                s->viewport[2] = sx * 2;
                s->viewport[3] = sy * 2;
            } else if (index == 0x82 || index == 0x84) {
                unsigned l = (index - 0x82) / 2;
                for (unsigned j = 0; j < 3; j++)
                    s->lookat[l][j] = (float)(int8_t)load(s, a + 8 + j, 1) / 127;
            } else if (index >= 0x86 && index <= 0x96) {
                unsigned l = (index - 0x86) / 2;
                if (l > 8)
                    return false;
                for (unsigned j = 0; j < 3; j++) {
                    s->lights[l][j] = (float)load(s, a + j, 1) / 255;
                    if (l < 8)
                        s->directions[l][j] = (float)(int8_t)load(s, a + 8 + j, 1) / 127;
                }
            } else
                return false;
            return true;
        }
        case 0xbc: {
            unsigned type = w0 & 255, offset = (w0 >> 8) & 65535;
            if (type == 6 || type == 14 || type == 4)
                return true;
            if (type == 2) {
                unsigned n = ((w1 - 0x80000000u) >> 5) - 1;
                if (n > 8)
                    return false;
                s->light_count = n;
                return true;
            }
            if (type == 8) {
                s->fog_mul = (int16_t)(w1 >> 16);
                s->fog_offset = (int16_t)w1;
                return true;
            }
            if (type == 10) {
                unsigned l = offset / 32;
                if (l > 8)
                    return false;
                for (unsigned j = 0; j < 3; j++)
                    s->lights[l][j] = (float)((w1 >> (24 - j * 8)) & 255) / 255;
                return true;
            }
            return false;
        }
        case 0xb6:
            s->geometry &= ~w1;
            return true;
        case 0xb7:
            s->geometry |= w1;
            return true;
        case 0xbb:
            s->texture_enabled = (w0 & 255) != 0;
            s->texture_tile = (w0 >> 8) & 7;
            s->texture_s = (float)(w1 >> 16) / 65536;
            s->texture_t = (float)(w1 & 65535) / 65536;
            s->textures_dirty = true;
            return true;
        case 0xbf: {
            unsigned a = (w1 >> 16) & 255, b = (w1 >> 8) & 255, c = w1 & 255;
            if (a % 10 || b % 10 || c % 10)
                return false;
            triangle(s, a / 10, b / 10, c / 10);
            return true;
        }
        case 0xbe:
            return true; /* Conservative culling: GLES clips the submitted geometry. */
        case 0xba:
        case 0xb9: {
            unsigned shift = (w0 >> 8) & 255, len = w0 & 255;
            if (!len || shift >= 32 || len > 32 - shift)
                return false;
            uint32_t mask = (len == 32 ? UINT32_MAX : ((1u << len) - 1)) << shift;
            uint32_t *mode = op == 0xba ? &s->other_high : &s->other_low;
            *mode = (*mode & ~mask) | (w1 & mask);
            s->textures_dirty = true;
            return true;
        }
        case 0xb4:
            s->half1 = w1;
            if (s->rect_stage == 1)
                s->rect_stage = 2;
            return true;
        case 0xb3:
            if (s->rect_stage == 1) {
                s->half1 = w1;
                s->rect_stage = 2;
                return true;
            }
            /* Original SM64 Fast3D uses B3/B2; later variants use B4/B3. */
            /* fall through */
        case 0xb2:
            if (s->rect_stage == 2) {
                unsigned cycle = (s->other_high >> 20) & 3;
                float x0 = (float)((s->rect1 >> 12) & 4095) / 4, y0 = (float)(s->rect1 & 4095) / 4,
                      x1 = (float)((s->rect0 >> 12) & 4095) / 4, y1 = (float)(s->rect0 & 4095) / 4;
                float du = (float)(int16_t)(w1 >> 16) / 1024, dv = (float)(int16_t)w1 / 1024;
                if (cycle == 2) {
                    du /= 4;
                    x1++;
                    y1++;
                }
                unsigned tile = s->texture_tile;
                s->texture_tile = (s->rect1 >> 24) & 7;
                bool enabled = s->texture_enabled;
                s->texture_enabled = true;
                s->textures_dirty = true;
                rectangle(s, x0, y0, x1, y1, (float)(int16_t)(s->half1 >> 16) / 32, (float)(int16_t)s->half1 / 32, du,
                          dv, s->rect_flip);
                s->texture_enabled = enabled;
                s->texture_tile = tile;
                s->textures_dirty = true;
                s->rect_stage = 0;
            }
            return true;
        case 0xe4:
        case 0xe5:
            s->rect0 = w0;
            s->rect1 = w1;
            s->rect_flip = op == 0xe5;
            s->rect_stage = 1;
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
            load_texture(s, &s->tiles[(w1 >> 24) & 7], w0, w1, op == 0xf3, op == 0xf0);
            return true;
        case 0xfd:
            s->image_fmt = (w0 >> 21) & 7;
            s->image_siz = (w0 >> 19) & 3;
            s->image_width = (w0 & 4095) + 1;
            s->image_address = span(s, w1, 1);
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
            return true;
        case 0xff: {
            uint32_t address = span(s, w1, 1);
            s->clearing_depth = address == s->depth_address;
            if (!s->clearing_depth) {
                if (s->recording) {
                    if (s->frame_color && s->frame_color != address)
                        s->midpoint_valid = false;
                    s->frame_color = address;
                }
                s->color_address = address;
                bind_surface(s);
            }
            return ((w0 >> 21) & 7) == 0 && ((w0 >> 19) & 3) == 2 && (w0 & 4095) == 319;
        }
        case 0xf6: {
            unsigned x0 = ((w1 >> 12) & 4095) / 4, y0 = (w1 & 4095) / 4, x1 = ((w0 >> 12) & 4095) / 4,
                     y1 = (w0 & 4095) / 4, cycle = (s->other_high >> 20) & 3;
            if (cycle >= 2) {
                x1++;
                y1++;
            }
            if (s->clearing_depth) {
                s->depth_clear = true;
                return true;
            }
            if (cycle == 3) {
                uint8_t p[4];
                pixel16(s->fill_color & 65535, p, false);
                s->gl.Enable(GL_SCISSOR_TEST);
                s->gl.Scissor((GLint)(x0 * s->render_scale), (GLint)((240 - y1) * s->render_scale),
                              (GLsizei)((x1 - x0) * s->render_scale), (GLsizei)((y1 - y0) * s->render_scale));
                s->gl.ClearColor((float)p[0] / 255, (float)p[1] / 255, (float)p[2] / 255, (float)p[3] / 255);
                s->gl.Clear(GL_COLOR_BUFFER_BIT);
                if (s->recording) {
                    Surface *mid = &s->midpoints[s->surface - s->surfaces];
                    s->gl.BindFramebuffer(GL_FRAMEBUFFER, mid->framebuffer);
                    s->gl.Clear(GL_COLOR_BUFFER_BIT);
                    s->gl.BindFramebuffer(GL_FRAMEBUFFER, s->surface->framebuffer);
                }
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
            fprintf(stderr, "GLES2 unsupported graphics command %02x at %08x (%08x %08x)\n", op, cmd->address, w0, w1);
            return false;
    }
}

RecompFast3DGles2 *recomp_fast3d_gles2_create(Runtime *rt, const char *title) {
    SDL_SetMainReady();
    if (!rt || !rt->rdram || SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
        return NULL;
    SDL_GL_ResetAttributes();
#ifdef __linux__
    SDL_SetHint(SDL_HINT_VIDEO_X11_FORCE_EGL, "1");
#endif
    if (SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES) ||
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2) || SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0) ||
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1))
        return NULL;
    RecompFast3DGles2 *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->rt = rt;
    s->owner = SDL_ThreadID();
#if defined(__aarch64__) || defined(_M_ARM64)
    s->render_scale = 1;
#else
    s->render_scale = 3;
#endif
    const char *scale = getenv("RECOMP_GLES2_SCALE");
    if (scale && *scale) {
        char *end;
        unsigned long value = strtoul(scale, &end, 10);
        if (*end || value < 1 || value > 4)
            recomp_fail("RECOMP_GLES2_SCALE must be 1..4", 0);
        s->render_scale = (unsigned)value;
    }
    s->color_gamma = 1.2f;
    const char *gamma = getenv("RECOMP_GLES2_GAMMA");
    if (gamma && *gamma) {
        char *end;
        float value = strtof(gamma, &end);
        if (*end || !isfinite(value) || value < 0.5f || value > 2.5f)
            recomp_fail("RECOMP_GLES2_GAMMA must be 0.5..2.5", 0);
        s->color_gamma = value;
    }
    s->window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 720,
                                 SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (s->window)
        s->context = SDL_GL_CreateContext(s->window);
    if (!s->context) {
        fprintf(stderr, "GLES2 context: %s\n", SDL_GetError());
        recomp_fast3d_gles2_destroy(s);
        return NULL;
    }
#define LOAD(name, type)                                                                                               \
    {                                                                                                                  \
        void *entry = SDL_GL_GetProcAddress("gl" #name);                                                               \
        _Static_assert(sizeof(entry) == sizeof(s->gl.name), "GL pointer size");                                        \
        if (!entry)                                                                                                    \
            recomp_fail("missing GLES2 gl" #name, 0);                                                                  \
        memcpy(&s->gl.name, &entry, sizeof(entry));                                                                    \
    }
    GL_FUNCTIONS(LOAD)
#undef LOAD
    s->gl.GenBuffers(1, &s->buffer);
    s->gl.ActiveTexture(GL_TEXTURE0);
    s->gl.GenTextures(1, &s->white);
    s->gl.BindTexture(GL_TEXTURE_2D, s->white);
    const uint8_t white[] = {255, 255, 255, 255};
    s->gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, white);
    s->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    s->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    s->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    s->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    identity(s->projection);
    identity(s->modelview[0]);
    s->texture_s = s->texture_t = 1;
    s->light_count = 1;
    for (unsigned j = 0; j < 3; j++) {
        s->prim[j] = s->env[j] = 1;
        s->lights[1][j] = 1;
    }
    s->prim[3] = s->env[3] = 1;
    s->lookat[0][0] = s->lookat[1][1] = 1;
    s->viewport[2] = 320;
    s->viewport[3] = 240;
    s->scissor[2] = 320;
    s->scissor[3] = 240;
    s->mux0 = 0xffffff;
    s->mux1 = (15u << 28) | (15u << 24) | (7u << 21) | (7u << 18) | (4u << 15) | (7u << 12) | (4u << 9) | (4u << 6) |
              (7u << 3) | 4;
    s->textures_dirty = true;
    bind_surface(s);
    const char *vs =
        "attribute vec4 position;attribute vec2 uv;varying vec2 tc;void main(){gl_Position=position;tc=uv;}";
    const char *fs = "precision mediump float;varying vec2 tc;uniform sampler2D image;uniform float colorGamma;"
                     "void main(){vec4 c=texture2D(image,tc);if(colorGamma!=1.0)c.rgb=pow(c.rgb,vec3(colorGamma));"
                     "gl_FragColor=c;}";
    GLuint v = shader(s, GL_VERTEX_SHADER, vs), f = shader(s, GL_FRAGMENT_SHADER, fs);
    s->blit = s->gl.CreateProgram();
    s->gl.AttachShader(s->blit, v);
    s->gl.AttachShader(s->blit, f);
    s->gl.BindAttribLocation(s->blit, 0, "position");
    s->gl.BindAttribLocation(s->blit, 2, "uv");
    s->gl.LinkProgram(s->blit);
    s->gl.DeleteShader(v);
    s->gl.DeleteShader(f);
    GLint ok;
    s->gl.GetProgramiv(s->blit, GL_LINK_STATUS, &ok);
    if (!ok)
        recomp_fail("GLES2 presentation shader link failed", 0);
    s->blit_gamma = s->gl.GetUniformLocation(s->blit, "colorGamma");
    s->input = recomp_input_create();
    if (!s->input)
        recomp_fail("GLES2 controller initialization failed", 0);
    (void)SDL_GL_SetSwapInterval(0);
    return s;
}
static void blit_texture(RecompFast3DGles2 *s, GLuint texture, GLuint framebuffer, int x, int y, int width, int height,
                         bool color_correct) {
    s->gl.BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    s->gl.Disable(GL_SCISSOR_TEST);
    s->gl.Disable(GL_DEPTH_TEST);
    s->gl.Disable(GL_BLEND);
    s->gl.Disable(GL_CULL_FACE);
    s->gl.Disable(GL_POLYGON_OFFSET_FILL);
    s->gl.Viewport(x, y, width, height);
    s->gl.UseProgram(s->blit);
    s->gl.Uniform1f(s->blit_gamma, color_correct ? s->color_gamma : 1.0f);
    s->gl.ActiveTexture(GL_TEXTURE0);
    s->gl.BindTexture(GL_TEXTURE_2D, texture);
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
void recomp_fast3d_gles2_draw(RecompFast3DGles2 *s, uint32_t root) {
    owner(s);
    s->frame++;
    s->recording = s->interpolate;
    s->identity_frame =
        s->recording && s->vertex_identity.begin && s->vertex_identity.begin(s->vertex_identity.userdata);
    s->identity_seen = false;
    s->midpoint_valid = s->identity_frame || s->history_valid;
    s->midpoint_changed = false;
    s->history_full = false;
    s->frame_color = 0;
    s->history_count = 0;
    s->topology = UINT64_C(1469598103934665603);
    s->full_sync = false;
    s->rect_stage = 0;
    memset(s->valid_vertices, 0, sizeof(s->valid_vertices));
    bind_surface(s);
    recomp_fast3d_walk(s->rt, &s->frontend, root, 1000000, consume, s);
    flush(s);
    if (s->rect_stage)
        recomp_fail("incomplete GLES2 texture rectangle", root);
    s->recording = false;
    s->surface->frame = s->frame;
    bool correspondence = s->identity_frame
                              ? s->identity_seen
                              : s->history_count == s->previous_count && s->topology == s->previous_topology;
    s->surface->midpoint = s->interpolate && s->full_sync && s->midpoint_valid && s->midpoint_changed && correspondence;
    if (s->interpolate) {
        VertexHistory *previous = s->history[0];
        s->history[0] = s->history[1];
        s->history[1] = previous;
        s->history_valid = s->full_sync && !s->history_full;
        s->previous_count = s->history_count;
        s->previous_topology = s->topology;
    }
    if (s->full_sync) {
        /* ReadPixels completes the color image before DP completion. Preserve
         * CPU-visible RAM, with one checked destination span for the copy. */
        if (s->color_address) {
            uint8_t *pixels = malloc(320 * 240 * 4);
            if (!pixels)
                recomp_fail("GLES2 color readback allocation failed", root);
            uint32_t target = recomp_ram_address_write_slow(s->rt, 0x80000000u | s->color_address, 320 * 240 * 2);
            if (target & 1)
                recomp_fail("unaligned GLES2 color image", s->color_address);
            if (s->render_scale > 1) {
                if (!s->readback.valid)
                    create_surface(s, &s->readback, 320, 240, false);
                /* Guest framebuffer addresses and sizes remain native. Only
                 * this downsampled image crosses the GPU-to-CPU boundary. */
                blit_texture(s, s->surface->texture, s->readback.framebuffer, 0, 0, 320, 240, false);
            }
            s->gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
            s->gl.ReadPixels(0, 0, 320, 240, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            for (unsigned y = 0; y < 240; y++)
                for (unsigned x = 0; x < 320; x++) {
                    const uint8_t *p = pixels + ((239 - y) * 320 + x) * 4;
                    uint16_t value =
                        (uint16_t)((p[0] >> 3) << 11 | (p[1] >> 3) << 6 | (p[2] >> 3) << 1 | (p[3] >= 128));
                    uint32_t offset = target + (y * 320 + x) * 2;
#if RECOMP_LITTLE_ENDIAN_HOST
                    memcpy(s->rt->rdram + (offset ^ 2), &value, sizeof(value));
#else
                    s->rt->rdram[offset] = (uint8_t)(value >> 8);
                    s->rt->rdram[offset + 1] = (uint8_t)value;
#endif
                }
            free(pixels);
            bind_surface(s);
        } else
            s->gl.Finish();
    }
}
void recomp_fast3d_gles2_set_framebuffer(RecompFast3DGles2 *s, uint32_t address) {
    owner(s);
    s->present_address = address;
}
void recomp_fast3d_gles2_set_frame_rate(RecompFast3DGles2 *s, uint32_t fps) {
    owner(s);
    if (s->frame || (fps != 0 && fps != 30 && fps != 60))
        recomp_fail("GLES2 frame rate must be configured as native or 60 before drawing", fps);
    s->interpolate = fps == 60;
    if (s->interpolate && !s->history[0]) {
        for (unsigned i = 0; i < 2; i++) {
            s->history[i] = calloc(HISTORY_SIZE, sizeof(VertexHistory));
            if (!s->history[i])
                recomp_fail("GLES2 interpolation history allocation failed", 0);
        }
    }
}
void recomp_fast3d_gles2_set_vertex_identity(RecompFast3DGles2 *s, const RecompVertexIdentity *identity) {
    owner(s);
    if (s->frame || !identity || !identity->begin || !identity->identify)
        recomp_fail("configure complete GLES2 vertex identity callbacks before drawing", 0);
    s->vertex_identity = *identity;
    if (!s->identity_history) {
        s->identity_history = calloc(HISTORY_SIZE, sizeof(IdentityHistory));
        if (!s->identity_history)
            recomp_fail("GLES2 vertex identity history allocation failed", 0);
    }
}
bool recomp_fast3d_gles2_present(RecompFast3DGles2 *s) {
    owner(s);
    flush(s);
    Surface *image = s->surface;
    for (unsigned i = 0; i < 8; i++)
        if (s->surfaces[i].valid && s->surfaces[i].address == s->present_address)
            image = &s->surfaces[i];
    /* Show (previous,current)/2 first, then current on the next display tick.
     * Never interpolate across a skipped/reordered VI framebuffer. Native
     * rendering and guest readback are independent of this presentation delay. */
    bool midpoint = image->midpoint && image->frame == s->presented_frame + 1;
    s->presented_frame = image->frame;
    if (midpoint)
        image = &s->midpoints[image - s->surfaces];
    s->gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    int width, height;
    SDL_GL_GetDrawableSize(s->window, &width, &height);
    s->gl.Disable(GL_SCISSOR_TEST);
    s->gl.ClearColor(0, 0, 0, 1);
    s->gl.Clear(GL_COLOR_BUFFER_BIT);
    int w = width, h = width * 3 / 4;
    if (h > height) {
        h = height;
        w = height * 4 / 3;
    }
    blit_texture(s, image->texture, 0, (width - w) / 2, (height - h) / 2, w, h, true);
    SDL_GL_SwapWindow(s->window);
    bind_surface(s);
    s->textures_dirty = true;
    return midpoint;
}
bool recomp_fast3d_gles2_poll(RecompFast3DGles2 *s) {
    owner(s);
    SDL_Event event;
    while (SDL_PollEvent(&event))
        if (event.type == SDL_QUIT || (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE))
            return false;
    return true;
}
bool recomp_fast3d_gles2_input(RecompFast3DGles2 *s, RecompPad *pad) {
    if (!recomp_fast3d_gles2_poll(s))
        return false;
    *pad = (RecompPad){.connected = true};
    if (!(SDL_GetWindowFlags(s->window) & SDL_WINDOW_INPUT_FOCUS))
        return true;
    const uint8_t *key = SDL_GetKeyboardState(NULL);
    const struct {
        SDL_Scancode key;
        uint16_t button;
    } map[] = {
        {SDL_SCANCODE_A, 0x8000}, {SDL_SCANCODE_RETURN, 0x8000}, {SDL_SCANCODE_X, 0x4000}, {SDL_SCANCODE_Z, 0x2000},
        {SDL_SCANCODE_S, 0x1000}, {SDL_SCANCODE_C, 0x20},        {SDL_SCANCODE_V, 0x10},   {SDL_SCANCODE_I, 8},
        {SDL_SCANCODE_K, 4},      {SDL_SCANCODE_J, 2},           {SDL_SCANCODE_L, 1}};
    for (unsigned i = 0; i < sizeof(map) / sizeof(*map); i++)
        if (key[map[i].key])
            pad->buttons |= map[i].button;
    pad->stick_x = (int8_t)((key[SDL_SCANCODE_RIGHT] - key[SDL_SCANCODE_LEFT]) * 80);
    pad->stick_y = (int8_t)((key[SDL_SCANCODE_UP] - key[SDL_SCANCODE_DOWN]) * 80);
    if (pad->stick_x && pad->stick_y) {
        pad->stick_x = (int8_t)(pad->stick_x * 7 / 10);
        pad->stick_y = (int8_t)(pad->stick_y * 7 / 10);
    }
    recomp_input_poll(s->input, pad);
    return true;
}
bool recomp_fast3d_gles2_full_sync(RecompFast3DGles2 *s) {
    return s->full_sync;
}
uint64_t recomp_fast3d_gles2_triangles(RecompFast3DGles2 *s) {
    return s->triangles;
}
bool recomp_fast3d_gles2_capture(RecompFast3DGles2 *s, const char *path) {
    owner(s);
    flush(s);
    Surface *f = s->surface;
    for (unsigned i = 0; i < 8; i++)
        if (s->surfaces[i].valid && s->surfaces[i].address == s->present_address)
            f = &s->surfaces[i];
    unsigned width = 320 * s->render_scale, height = 240 * s->render_scale;
    if (s->color_gamma != 1.0f) {
        if (!s->capture.valid)
            create_surface(s, &s->capture, width, height, false);
        blit_texture(s, f->texture, s->capture.framebuffer, 0, 0, (int)width, (int)height, true);
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
    bind_surface(s);
    return ok;
}
void recomp_fast3d_gles2_destroy(RecompFast3DGles2 *s) {
    if (!s)
        return;
    if (s->context) {
        owner(s);
        if (s->input)
            recomp_input_destroy(s->input);
        if (s->buffer)
            s->gl.DeleteBuffers(1, &s->buffer);
        if (s->white)
            s->gl.DeleteTextures(1, &s->white);
        if (s->blit)
            s->gl.DeleteProgram(s->blit);
        for (unsigned i = 0; i < s->program_count; i++)
            s->gl.DeleteProgram(s->programs[i].id);
        for (unsigned i = 0; i < 128; i++) {
            if (s->textures[i].id)
                s->gl.DeleteTextures(1, &s->textures[i].id);
        }
        for (unsigned i = 0; i < 8; i++)
            if (s->surfaces[i].valid) {
                s->gl.DeleteTextures(1, &s->surfaces[i].texture);
                s->gl.DeleteFramebuffers(1, &s->surfaces[i].framebuffer);
                s->gl.DeleteRenderbuffers(1, &s->surfaces[i].depth);
            }
        for (unsigned i = 0; i < 8; i++)
            if (s->midpoints[i].valid) {
                s->gl.DeleteTextures(1, &s->midpoints[i].texture);
                s->gl.DeleteFramebuffers(1, &s->midpoints[i].framebuffer);
                s->gl.DeleteRenderbuffers(1, &s->midpoints[i].depth);
            }
        if (s->readback.valid) {
            s->gl.DeleteTextures(1, &s->readback.texture);
            s->gl.DeleteFramebuffers(1, &s->readback.framebuffer);
        }
        if (s->capture.valid) {
            s->gl.DeleteTextures(1, &s->capture.texture);
            s->gl.DeleteFramebuffers(1, &s->capture.framebuffer);
        }
        SDL_GL_DeleteContext(s->context);
    }
    if (s->window)
        SDL_DestroyWindow(s->window);
    free(s->history[0]);
    free(s->history[1]);
    free(s->identity_history);
    free(s);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
}
