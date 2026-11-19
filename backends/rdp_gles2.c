#include "rdp_gles2.h"
#include "input.h"
#include "rdp_triangle.h"
#include <SDL.h>
#include <SDL_opengles2.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    X(Uniform2f, PFNGLUNIFORM2FPROC)                                                                                   \
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
    X(GetError, PFNGLGETERRORPROC)                                                                                     \
    X(GetString, PFNGLGETSTRINGPROC)                                                                                   \
    X(ReadPixels, PFNGLREADPIXELSPROC)                                                                                 \
    X(PixelStorei, PFNGLPIXELSTOREIPROC)                                                                               \
    X(GenTextures, PFNGLGENTEXTURESPROC)                                                                               \
    X(BindTexture, PFNGLBINDTEXTUREPROC)                                                                               \
    X(TexImage2D, PFNGLTEXIMAGE2DPROC)                                                                                 \
    X(TexParameteri, PFNGLTEXPARAMETERIPROC)                                                                           \
    X(GenerateMipmap, PFNGLGENERATEMIPMAPPROC)                                                                         \
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
    float position[4], color[4], uv[2], fog, reciprocal;
} Vertex;
typedef struct Tile {
    unsigned fmt, siz, line, tmem, palette, cs, ct, ms, mt, ss, st, sl, tl, sh, th;
} Tile;
typedef struct Program {
    uint32_t mux0, mux1, cycle;
    GLuint id;
    GLint prim, env, fog, lod, alpha, fogen, perspective;
    GLint tile[2][3];
    bool texture[2];
} Program;
typedef struct Texture {
    uint64_t hash, used;
    GLuint id;
} Texture;
typedef struct Surface {
    uint32_t address;
    unsigned width, height;
    GLuint framebuffer, texture, depth;
    bool valid;
} Surface;
typedef struct DepthSurface {
    uint32_t address;
    unsigned width, height;
    GLuint buffer;
    bool clear;
} DepthSurface;
struct RecompRDPGles2 {
    Runtime *rt;
    SDL_Window *window;
    SDL_GLContext context;
    SDL_threadID owner;
    RecompInput *input;
    RecompGles2InputConfig input_config;
    RecompMouseFrame mouse_frame;
    RecompCameraStick camera_stick;
    bool mouse_captured, mouse_released, expanded_aspect, anamorphic;
    struct {
#define DECLARE(name, type) type name;
        GL_FUNCTIONS(DECLARE)
#undef DECLARE
    } gl;
    GLuint buffer, white, blit;
    GLenum depth_format;
    GLint blit_gamma, blit_pixel, blit_aa;
    bool antialias, mipmaps, texture_gradients, core_gradients;
    Program programs[256];
    unsigned program_count;
    Texture textures[128];
    uint64_t texture_clock;
    GLuint active_texture[2];
    unsigned tex_width[2], tex_height[2];
    bool textures_dirty;
    Surface surfaces[8];
    DepthSurface depths[16];
    Surface readback, capture, cpu_image;
    Surface *surface;
    unsigned render_scale;
    unsigned width, height, color_siz;
    const RecompFramebufferSize *framebuffer_sizes;
    size_t framebuffer_size_count;
    float color_gamma;
    uint32_t color_address, depth_address, present_address;
    bool clearing_depth, rdp_perspective;
    Vertex batch[1536];
    unsigned batch_count;
    uint32_t other_high, other_low, mux0, mux1, fill_color, blend_color;
    bool depth_test, fog;
    float prim[4], env[4], fog_color[4], prim_lod;
    unsigned texture_tile;
    bool texture_enabled;
    Tile tiles[8];
    uint8_t tmem[4096], tmem_invalid[4096];
    unsigned image_fmt, image_siz, image_width;
    uint32_t image_address;
    float viewport[4], primitive_depth;
    unsigned scissor[4];
    bool full_sync, started;
    uint64_t triangles;
};
static void flush(RecompRDPGles2 *s);
static void owner(RecompRDPGles2 *s) {
    if (SDL_ThreadID() != s->owner || SDL_GL_GetCurrentContext() != s->context)
        recomp_fail("GLES2 requires its owner thread/context", 0);
}
static bool extension(const char *list, const char *name) {
    size_t length = strlen(name);
    for (const char *p = list; p && (p = strstr(p, name)); p += length)
        if ((p == list || p[-1] == ' ') && (!p[length] || p[length] == ' '))
            return true;
    return false;
}
static bool quality_option(const char *name, bool fallback) {
    const char *value = getenv(name);
    if (!value || !*value)
        return fallback;
    if (strcmp(value, "0") && strcmp(value, "1"))
        recomp_fail("GLES2 quality switches accept 0 or 1", 0);
    return value[0] == '1';
}

static uint32_t span(RecompRDPGles2 *s, uint32_t a, size_t n) {
    return recomp_ram_address(s->rt, 0x80000000u | (a & 0x03ffffff), n);
}
static uint32_t load(RecompRDPGles2 *s, uint32_t a, unsigned n) {
    /* Callers have already resolved and checked the complete physical span.
     * Graphics RAM is not a CPU/device access; avoid the generic bus decoder
     * for every texture byte and vertex component. */
#if RECOMP_LITTLE_ENDIAN_HOST
    if (n == 1)
        return s->rt->rdram[a ^ 3];
    if (a & 1)
        recomp_fail("unaligned RDP halfword", a);
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
static GLuint shader(RecompRDPGles2 *s, GLenum type, const char *text) {
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
static void texture_use(Program *p, unsigned item, bool rgb_multiplier, bool second_cycle) {
    if (second_cycle) {
        if (item == 1 || item == 2)
            item = 3 - item;
        else if (rgb_multiplier && (item == 8 || item == 9))
            item = 17 - item;
    }
    if (item == 1 || item == 2)
        p->texture[item - 1] = true;
    if (rgb_multiplier && (item == 8 || item == 9))
        p->texture[item - 8] = true;
}
static Program *program(RecompRDPGles2 *s) {
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
    const char *vs = "attribute vec4 position;attribute vec4 color;attribute vec2 uv;attribute float fogv;attribute "
                     "float reciprocal;"
                     "varying vec4 shade;varying vec2 texcoord;varying float fogfactor;varying float invw;"
                     "void main(){gl_Position=position;shade=color;texcoord=uv;fogfactor=fogv;invw=reciprocal;}";
    if (s->core_gradients)
        vs = "#version 300 es\nin vec4 position;in vec4 color;in vec2 uv;in float fogv;in float reciprocal;"
             "out vec4 shade;out vec2 texcoord;out float fogfactor;out float invw;"
             "void main(){gl_Position=position;shade=color;texcoord=uv;fogfactor=fogv;invw=reciprocal;}";
    char fs[8192];
    int pos = snprintf(
        fs, sizeof(fs),
        "%s#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n"
        "varying vec4 shade;varying vec2 texcoord;varying float fogfactor;varying float invw;uniform vec4 "
        "prim,env,fogc;uniform "
        "float lod,alphatest,fogen,rdpPerspective;"
        "uniform sampler2D tex0,tex1;uniform vec4 dim0,dim1,origin0,origin1,wrap0,wrap1;"
        "vec2 coord(vec2 uv,vec4 d,vec4 o,vec4 w){vec2 p=uv*d.zw-o.xy;"
        "if(o.z<0.5){if(w.x>0.0){p.x=mod(p.x,w.x*(w.z>0.0?2.0:1.0));"
        "if(w.z>0.0)p.x=min(p.x,w.x*2.0-1.0-p.x);}p.x=clamp(p.x,0.0,d.x-1.0);}"
        "if(o.w<0.5){if(w.y>0.0){p.y=mod(p.y,w.y*(w.w>0.0?2.0:1.0));"
        "if(w.w>0.0)p.y=min(p.y,w.y*2.0-1.0-p.y);}p.y=clamp(p.y,0.0,d.y-1.0);}"
        "return (p+vec2(0.5))/d.xy;}"
        "void main(){vec2 sourceUV=texcoord;"
        "if(rdpPerspective>0.5){sourceUV=invw<=0.0?vec2(1023.96875):"
        "clamp(texcoord/invw*16384.0,vec2(-1024.0),vec2(1023.96875));}%s"
        "float noise=fract(sin(dot(gl_FragCoord.xy,vec2(12.9898,78.233)))*43758.5);vec4 combined=vec4(0.0);",
        s->core_gradients ? "#version 300 es\n#define varying in\n#define texture2D texture\n"
                            "#define texture2DGradEXT textureGrad\nout highp vec4 outputColor;\n"
                            "#define gl_FragColor outputColor\n"
        : s->texture_gradients
            ? "#extension GL_OES_standard_derivatives : enable\n#extension GL_EXT_shader_texture_lod : enable\n"
            : "",
        s->texture_gradients
            ? "vec2 dx=dFdx(sourceUV),dy=dFdy(sourceUV);"
              "vec4 t0=texture2DGradEXT(tex0,coord(sourceUV,dim0,origin0,wrap0),dx*dim0.zw/dim0.xy,dy*dim0.zw/dim0.xy);"
              "vec4 t1=texture2DGradEXT(tex1,coord(sourceUV,dim1,origin1,wrap1),dx*dim1.zw/dim1.xy,dy*dim1.zw/dim1.xy);"
            : "vec4 t0=texture2D(tex0,coord(sourceUV,dim0,origin0,wrap0));"
              "vec4 t1=texture2D(tex1,coord(sourceUV,dim1,origin1,wrap1));");
    if (cycle == 2) {
        pos += snprintf(fs + pos, sizeof(fs) - (size_t)pos, "combined=t0;");
        p->texture[0] = true;
    }
    for (unsigned c = cycle == 1 ? 0 : 1; c < 2 && cycle != 2; c++) {
        bool second_cycle = cycle == 1 && c == 1;
        if (second_cycle)
            pos += snprintf(fs + pos, sizeof(fs) - (size_t)pos, "vec4 previousTexel=t0;t0=t1;t1=previousTexel;");
        unsigned a = c ? (s->mux0 >> 5) & 15 : (s->mux0 >> 20) & 15, b = c ? (s->mux1 >> 24) & 15 : s->mux1 >> 28;
        unsigned m = c ? s->mux0 & 31 : (s->mux0 >> 15) & 31, d = c ? (s->mux1 >> 6) & 7 : (s->mux1 >> 15) & 7;
        unsigned aa = c ? (s->mux1 >> 21) & 7 : (s->mux0 >> 12) & 7, ab = c ? (s->mux1 >> 3) & 7 : (s->mux1 >> 12) & 7;
        unsigned ac = c ? (s->mux1 >> 18) & 7 : (s->mux0 >> 9) & 7, ad = c ? s->mux1 & 7 : (s->mux1 >> 9) & 7;
        texture_use(p, d, false, second_cycle);
        texture_use(p, ad, false, second_cycle);
        if (m != 31 && a != b) {
            texture_use(p, a, false, second_cycle);
            texture_use(p, b, false, second_cycle);
            texture_use(p, m, true, second_cycle);
        }
        if (ac != 7 && aa != ab) {
            texture_use(p, aa, false, second_cycle);
            texture_use(p, ab, false, second_cycle);
            texture_use(p, ac, false, second_cycle);
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
    const char *attrs[] = {"position", "color", "uv", "fogv", "reciprocal"};
    for (unsigned i = 0; i < 5; i++)
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
    p->perspective = s->gl.GetUniformLocation(p->id, "rdpPerspective");
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
static void create_surface(RecompRDPGles2 *s, Surface *f, unsigned width, unsigned height) {
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
static void destroy_surface(RecompRDPGles2 *s, Surface *f) {
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
            destroy_surface(s, f);
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
    create_surface(s, f, s->width * s->render_scale, s->height * s->render_scale);
    return f;
}
/* N64 color images can rotate while retaining the same physical Z image.
 * Keep depth storage independent of color FBO ownership and task boundaries. */
static DepthSurface *depth_surface(RecompRDPGles2 *s, unsigned width, unsigned height) {
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
    s->gl.RenderbufferStorage(GL_RENDERBUFFER, s->depth_format, (GLsizei)width, (GLsizei)height);
    return available;
}
static void bind_surface(RecompRDPGles2 *s) {
    s->surface = surface(s, s->color_address);
    s->gl.BindFramebuffer(GL_FRAMEBUFFER, s->surface->framebuffer);
    DepthSurface *depth = depth_surface(s, s->surface->width, s->surface->height);
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
static unsigned byte(RecompRDPGles2 *s, unsigned offset) {
    if (s->tmem_invalid[offset & 4095])
        recomp_fail("GLES2 drawing consumes an invalid texture DMA", offset & 4095);
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
static GLuint texture(RecompRDPGles2 *s, Tile *t, unsigned *width, unsigned *height) {
    *width = t->sh >= t->sl ? (t->sh - t->sl) / 4 + 1 : 1;
    *height = t->th >= t->tl ? (t->th - t->tl) / 4 + 1 : 1;
    /* Wrapped coordinates cannot observe texels beyond their mask period.
     * Excluding that unused TMEM prevents unrelated bytes entering mip levels. */
    if (t->ms && !(t->cs & 2) && (1u << t->ms) < *width)
        *width = 1u << t->ms;
    if (t->mt && !(t->ct & 2) && (1u << t->mt) < *height)
        *height = 1u << t->mt;
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
            hash ^= (unsigned)s->tmem[i] | ((unsigned)s->tmem_invalid[i] << 8);
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
            unsigned b0 = byte(s, o & mask), b1 = t->siz >= 2 ? byte(s, (o + 1) & mask) : 0,
                     nibble = (b0 >> ((x & 1) ? 0 : 4)) & 15;
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
    if (s->mipmaps && !(*width & (*width - 1)) && !(*height & (*height - 1)))
        s->gl.GenerateMipmap(GL_TEXTURE_2D);
    free(pixels);
    s->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    s->gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    entry->hash = hash;
    entry->used = ++s->texture_clock;
    return entry->id;
}
static void flush(RecompRDPGles2 *s) {
    if (!s->batch_count)
        return;
    Program *p = program(s);
    s->gl.UseProgram(p->id);
    s->gl.Uniform4fv(p->prim, 1, s->prim);
    s->gl.Uniform4fv(p->env, 1, s->env);
    s->gl.Uniform4fv(p->fog, 1, s->fog_color);
    s->gl.Uniform1f(p->lod, s->prim_lod);
    s->gl.Uniform1f(p->perspective, s->rdp_perspective ? 1.0f : 0.0f);
    bool alpha = (s->other_low & 0x1000) || (s->other_low & 3);
    s->gl.Uniform1f(p->alpha, alpha ? (s->other_low & 3 ? ((float)(s->blend_color & 255) / 255) : 0.5f) : -1);
    s->gl.Uniform1f(p->fogen, s->fog ? 1.0f : 0.0f);
    for (unsigned i = 0; i < 2; i++) {
        Tile *t = &s->tiles[(s->texture_tile + (p->cycle == 0 ? 0 : i)) & 7];
        s->gl.ActiveTexture(GL_TEXTURE0 + i);
        if (!s->texture_enabled || !p->texture[i]) {
            s->active_texture[i] = s->white;
            s->tex_width[i] = s->tex_height[i] = 1;
        } else if (s->textures_dirty)
            s->active_texture[i] = texture(s, t, &s->tex_width[i], &s->tex_height[i]);
        s->gl.BindTexture(GL_TEXTURE_2D, s->active_texture[i]);
        GLint filter = p->cycle == 2 ? GL_NEAREST : GL_LINEAR;
        GLint min_filter = filter;
        if (p->cycle != 2 && s->mipmaps && s->texture_enabled && p->texture[i] &&
            !(s->tex_width[i] & (s->tex_width[i] - 1)) && !(s->tex_height[i] & (s->tex_height[i] - 1)))
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
         * wrapping is still needed for masks inside larger/NPOT textures. */
        bool power_of_two = !(s->tex_width[i] & (s->tex_width[i] - 1)) && !(s->tex_height[i] & (s->tex_height[i] - 1));
        for (unsigned axis = 0; axis < 2; axis++) {
            bool repeat = power_of_two && wrap[axis] > 0 && wrap[axis] == dim[axis];
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
    if (s->other_low & 0x4000) {
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
    s->gl.DrawArrays(GL_TRIANGLES, 0, (GLsizei)s->batch_count);

    s->triangles += s->batch_count / 3;
    s->batch_count = 0;
}

/* SETTIMG only changes a register. Some lists load a sentinel image during
 * cleanup, then overwrite TMEM before it is sampled. Track undefined DMA bytes
 * until a texture actually observes them; never fabricate a drawable texture.
 * Sources are RDP physical/segmented addresses, independent of CPU TLB aliases. */
static bool texture_source(RecompRDPGles2 *s, uint64_t offset, size_t bytes, uint32_t *physical) {
    uint64_t base = s->image_address & 0x03ffffff;
    uint64_t source = base + offset;
    if (source >= s->rt->rdram_size || bytes > s->rt->rdram_size - source)
        return false;
    *physical = (uint32_t)source;
    return true;
}
static void tmem_store(RecompRDPGles2 *s, unsigned offset, bool valid, unsigned value) {
    offset &= 4095;
    s->tmem[offset] = (uint8_t)value;
    s->tmem_invalid[offset] = valid ? 0 : 255;
}
static void load_texture(RecompRDPGles2 *s, Tile *t, uint32_t w0, uint32_t w1, bool block, bool palette) {
    unsigned sl = (w0 >> 12) & 4095, tl = w0 & 4095, sh = (w1 >> 12) & 4095, th = w1 & 4095;
    unsigned bits = 4u << s->image_siz;
    /* LOADTILE also latches the tile's sampling bounds. Raw RDP video may
     * upload and draw through this same tile without a separate SETTILESIZE. */
    if (!block && !palette) {
        t->sl = sl;
        t->tl = tl;
        t->sh = sh;
        t->th = th;
    }
    if (palette) {
        if (sh < sl || th < tl)
            recomp_fail("GLES2 malformed palette load", w0);
        unsigned width = (sh >> 2) - (sl >> 2) + 1, height = (th >> 2) - (tl >> 2) + 1;
        for (unsigned y = 0; y < height; y++) {
            uint32_t src = 0;
            bool valid =
                texture_source(s, ((uint64_t)((tl >> 2) + y) * s->image_width + (sl >> 2)) * 2, width * 2, &src);
            for (unsigned i = 0; i < width; i++)
                for (unsigned lane = 0; lane < 4; lane++) {
                    unsigned dst = t->tmem * 8 + y * t->line * 32 + i * 8 + lane * 2;
                    tmem_store(s, dst, valid, valid ? load(s, src + i * 2, 1) : 0);
                    tmem_store(s, dst + 1, valid, valid ? load(s, src + i * 2 + 1, 1) : 0);
                }
        }
    } else if (block) {
        /* LOADBLOCK coordinates are integer texels, unlike LOADTILE/TLUT's
         * quarter-texel bounds. Each transfer reads a complete 64-bit word. */
        if (sh < sl)
            recomp_fail("GLES2 malformed texture block load", w0);
        unsigned words = ((sh - sl) >> (4 - t->siz)) + 1, bytes = words * 8;
        uint32_t src = 0;
        bool valid = texture_source(s, ((uint64_t)tl * s->image_width + sl) * bits / 8, bytes, &src);
        for (unsigned i = 0; i < bytes; i++) {
            unsigned word = i / 8, row = (word * th) >> 11;
            unsigned offset = t->tmem * 8 + i + row * t->line * 8;
            if (t->siz == 3) {
                unsigned pixel = i / 4, part = i & 3;
                offset = t->tmem * 8 + pixel * 2 + (part & 1) + row * t->line * 8;
                offset = (offset & 2047) | (part >= 2 ? 2048 : 0);
            }
            offset ^= (row & 1) ? 4 : 0;
            tmem_store(s, offset, valid, valid ? load(s, src + i, 1) : 0);
        }
    } else {
        unsigned width = (sh - sl) / 4 + 1, height = (th - tl) / 4 + 1;
        if (sh < sl || th < tl || width > 1024 || height > 1024)
            recomp_fail("GLES2 malformed texture tile load", w0);
        for (unsigned y = 0; y < height; y++)
            for (unsigned x = 0; x < width; x++) {
                uint64_t bit = ((uint64_t)((tl >> 2) + y) * s->image_width + (sl >> 2) + x) * bits;
                unsigned bpp = bits >= 8 ? bits / 8 : 1;
                uint32_t src = 0;
                bool valid = texture_source(s, bit / 8, bpp, &src);
                for (unsigned b = 0; b < bpp; b++) {
                    unsigned offset = t->tmem * 8 + y * t->line * 8 + ((x * bits) >> 3) + b;
                    if (bits == 32)
                        offset = ((t->tmem * 8 + y * t->line * 8 + x * 2 + (b & 1)) & 2047) | (b >= 2 ? 2048 : 0);
                    offset ^= (y & 1) ? 4 : 0;
                    offset &= 4095;
                    unsigned value = valid ? load(s, src + b, 1) : 0;
                    if (bits == 4) {
                        unsigned nibble = (value >> ((bit & 4) ? 0 : 4)) & 15, shift = (x & 1) ? 0 : 4;
                        unsigned mask = 15u << shift;
                        s->tmem[offset] = (uint8_t)((s->tmem[offset] & ~mask) | (nibble << shift));
                        s->tmem_invalid[offset] = (uint8_t)((s->tmem_invalid[offset] & ~mask) | (valid ? 0 : mask));
                    } else
                        tmem_store(s, offset, valid, value);
                }
            }
    }
    s->textures_dirty = true;
}
static void rectangle(RecompRDPGles2 *s, float x0, float y0, float x1, float y1, float u, float v, float du, float dv,
                      bool flip) {
    flush(s);
    bool perspective = s->rdp_perspective;
    s->rdp_perspective = false;
    float oldviewport[4];
    memcpy(oldviewport, s->viewport, sizeof(oldviewport));
    s->viewport[0] = s->viewport[1] = 0;
    s->viewport[2] = (float)s->width;
    s->viewport[3] = (float)s->height;
    bool depth = s->depth_test, fog = s->fog;
    s->depth_test = s->fog = false;
    Vertex q[4] = {0};
    float x[4] = {x0, x1, x1, x0}, y[4] = {y0, y0, y1, y1};
    for (unsigned i = 0; i < 4; i++) {
        q[i].position[0] = x[i] * 2 / (float)s->width - 1;
        q[i].position[1] = 1 - y[i] * 2 / (float)s->height;
        q[i].position[3] = 1;
        q[i].position[2] = s->other_low & 4 ? s->primitive_depth * 2 - 1 : 0;
        /* RDP rectangles supply zero shade coefficients. */
        q[i].uv[0] = u + (flip ? y[i] - y0 : x[i] - x0) * du;
        q[i].uv[1] = v + (flip ? x[i] - x0 : y[i] - y0) * dv;
    }
    const unsigned order[] = {0, 1, 2, 0, 2, 3};
    for (unsigned i = 0; i < 6; i++)
        s->batch[s->batch_count++] = q[order[i]];

    flush(s);
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
        x1++;
        y1++;
    }
    unsigned tile = s->texture_tile;
    s->texture_tile = (w1 >> 24) & 7;
    bool enabled = s->texture_enabled;
    s->texture_enabled = true;
    s->textures_dirty = true;
    rectangle(s, x0, y0, x1, y1, (float)(int16_t)(coordinates >> 16) / 32, (float)(int16_t)coordinates / 32, du, dv,
              flip);
    s->texture_enabled = enabled;
    s->texture_tile = tile;
    s->textures_dirty = true;
}
static bool consume_rdp(RecompRDPGles2 *s, uint32_t w0, uint32_t w1) {
    unsigned op = w0 >> 24;
    if (op != 0xc0)
        flush(s);
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
            s->primitive_depth = (float)(w1 >> 16) / 32768;
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
            load_texture(s, &s->tiles[(w1 >> 24) & 7], w0, w1, op == 0xf3, op == 0xf0);
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
                bind_surface(s);
            return true;
        case 0xff: {
            unsigned siz = (w0 >> 19) & 3;
            if (((w0 >> 21) & 7) != 0 || siz < 2)
                return false;
            unsigned width = (w0 & 4095) + 1, height = 0;
            for (size_t i = 0; i < s->framebuffer_size_count; i++)
                if (s->framebuffer_sizes[i].width == width)
                    height = s->framebuffer_sizes[i].height;
            if (!height)
                return false;
            s->width = width;
            s->height = height;
            uint32_t address = span(s, w1, 1);
            s->clearing_depth = address == s->depth_address;
            if (!s->clearing_depth) {
                s->color_address = address;
                s->color_siz = siz;
                bind_surface(s);
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
                depth_surface(s, s->width * s->render_scale, s->height * s->render_scale)->clear = true;
                return true;
            }
            if (cycle == 3) {
                uint8_t p[4];
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
    if (changed || s->batch_count + mesh.count > 1536)
        flush(s);
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

RecompRDPGles2 *recomp_rdp_gles2_create(Runtime *rt, const char *title) {
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
    RecompRDPGles2 *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->rt = rt;
    static const RecompFramebufferSize default_sizes[] = {{320, 240}};
    s->framebuffer_sizes = default_sizes;
    s->framebuffer_size_count = 1;
    s->width = 320;
    s->height = 240;
    s->color_siz = 2;
    s->owner = SDL_ThreadID();
    const char *expanded = getenv("RECOMP_EXPANDED_ASPECT");
    s->expanded_aspect = expanded && !strcmp(expanded, "1");
#if defined(__aarch64__) || defined(_M_ARM64)
    s->render_scale = 1;
    s->antialias = false;
#else
    s->render_scale = 4;
    s->antialias = true;
#endif
    s->antialias = quality_option("RECOMP_GLES2_AA", s->antialias);
    s->mipmaps = quality_option("RECOMP_GLES2_MIPMAPS", true);
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
    s->window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 960,
                                 SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (s->window)
        s->context = SDL_GL_CreateContext(s->window);
    if (!s->context) {
        fprintf(stderr, "GLES2 context: %s\n", SDL_GetError());
        recomp_rdp_gles2_destroy(s);
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
    const char *extensions = (const char *)s->gl.GetString(GL_EXTENSIONS);
    const char *version = (const char *)s->gl.GetString(GL_VERSION);
    unsigned major = 0, minor = 0;
    s->core_gradients = version && sscanf(version, "OpenGL ES %u.%u", &major, &minor) == 2 && major >= 3;
    /* RDP depth has 18 bits before compression; 16-bit linear GL depth loses
     * ordering between nearby world surfaces. Keep the legal GLES2 fallback. */
    s->depth_format =
        major >= 3 || extension(extensions, "GL_OES_depth24") ? GL_DEPTH_COMPONENT24_OES : GL_DEPTH_COMPONENT16;
    s->texture_gradients = s->core_gradients || (extension(extensions, "GL_OES_standard_derivatives") &&
                                                 extension(extensions, "GL_EXT_shader_texture_lod"));
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
    for (unsigned j = 0; j < 3; j++) {
        s->prim[j] = s->env[j] = 1;
    }
    s->prim[3] = s->env[3] = 1;
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
    const char *fs =
        "#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n"
        "varying vec2 tc;uniform sampler2D image;uniform float colorGamma,aa;"
        "uniform vec2 pixel;"
        "void main(){vec4 c=texture2D(image,tc);if(aa>0.0){"
        "vec3 nw=texture2D(image,tc+pixel*vec2(-1.0,-1.0)).rgb;"
        "vec3 ne=texture2D(image,tc+pixel*vec2(1.0,-1.0)).rgb;"
        "vec3 sw=texture2D(image,tc+pixel*vec2(-1.0,1.0)).rgb;"
        "vec3 se=texture2D(image,tc+pixel*vec2(1.0,1.0)).rgb;vec3 l=vec3(0.299,0.587,0.114);"
        "float lnw=dot(nw,l),lne=dot(ne,l),lsw=dot(sw,l),lse=dot(se,l),lm=dot(c.rgb,l);"
        "float lo=min(lm,min(min(lnw,lne),min(lsw,lse))),hi=max(lm,max(max(lnw,lne),max(lsw,lse)));"
        "vec2 d=vec2(-((lnw+lne)-(lsw+lse)),(lnw+lsw)-(lne+lse));"
        "float reduce=max((lnw+lne+lsw+lse)*0.03125,0.0078125);"
        "d=clamp(d/(min(abs(d.x),abs(d.y))+reduce),vec2(-8.0),vec2(8.0))*pixel;"
        "vec3 a=0.5*(texture2D(image,tc-d/6.0).rgb+texture2D(image,tc+d/6.0).rgb);"
        "vec3 b=a*0.5+0.25*(texture2D(image,tc-d*0.5).rgb+texture2D(image,tc+d*0.5).rgb);"
        "float lb=dot(b,l);c.rgb=(lb<lo||lb>hi)?a:b;}"
        "if(colorGamma!=1.0)c.rgb=pow(c.rgb,vec3(colorGamma));gl_FragColor=c;}";
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
    s->blit_pixel = s->gl.GetUniformLocation(s->blit, "pixel");
    s->blit_aa = s->gl.GetUniformLocation(s->blit, "aa");
#ifdef RECOMP_DEBUG
    fprintf(stderr, "GLES2: scale=%u, FXAA=%s, mipmaps=%s, texture gradients=%s\n", s->render_scale,
            s->antialias ? "on" : "off", s->mipmaps ? "on" : "off", s->texture_gradients ? "on" : "off");
#endif
    s->input = recomp_input_create();
    if (!s->input)
        recomp_fail("GLES2 controller initialization failed", 0);
    (void)SDL_GL_SetSwapInterval(0);
    return s;
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
static void draw_begin(RecompRDPGles2 *s) {
    s->full_sync = false;
    s->started = true;
    bind_surface(s);
}
static void draw_end(RecompRDPGles2 *s, uint32_t root) {
    flush(s);
    if (s->full_sync) {
        /* ReadPixels completes the color image before DP completion. Preserve
         * CPU-visible RAM, with one checked destination span for the copy. */
        if (s->color_address) {
            unsigned width = s->width, height = s->height;
            size_t pixels_count = (size_t)width * height;
            uint8_t *pixels = malloc(pixels_count * 4);
            if (!pixels)
                recomp_fail("GLES2 color readback allocation failed", root);
            unsigned bpp = s->color_siz == 3 ? 4 : 2;
            uint32_t target = recomp_ram_address_write_slow(s->rt, 0x80000000u | s->color_address, pixels_count * bpp);
            if (target & (bpp - 1))
                recomp_fail("unaligned GLES2 color image", s->color_address);
            if (s->render_scale > 1) {
                if (s->readback.valid && (s->readback.width != width || s->readback.height != height))
                    destroy_surface(s, &s->readback);
                if (!s->readback.valid)
                    create_surface(s, &s->readback, width, height);
                /* Guest framebuffer addresses and sizes remain native. Only
                 * this downsampled image crosses the GPU-to-CPU boundary. */
                blit_texture(s, s->surface, s->readback.framebuffer, 0, 0, (int)width, (int)height, false);
            }
            s->gl.PixelStorei(GL_PACK_ALIGNMENT, 1);
            s->gl.ReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
            for (unsigned y = 0; y < height; y++)
                for (unsigned x = 0; x < width; x++) {
                    const uint8_t *p = pixels + ((size_t)(height - y - 1) * width + x) * 4;
                    uint32_t offset = target + (y * width + x) * bpp;
                    if (bpp == 4) {
                        uint32_t value = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
                        memcpy(s->rt->rdram + offset, &value, sizeof(value));
                        continue;
                    }
                    uint16_t value =
                        (uint16_t)((p[0] >> 3) << 11 | (p[1] >> 3) << 6 | (p[2] >> 3) << 1 | (p[3] >= 128));
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

void recomp_rdp_gles2_draw_rdp(RecompRDPGles2 *s, uint8_t *ram, size_t ram_size, const uint32_t *words, size_t count) {
    owner(s);
    if (!ram || ram_size != s->rt->rdram_size || (!words && count) || count > 4 * 1024 * 1024)
        recomp_fail("invalid native RSP RDP output", 0);
    Runtime *original = s->rt;
    Runtime view = {.rdram = ram, .rdram_size = ram_size};
    s->rt = &view;
    s->rdp_perspective = false;
    draw_begin(s);
    for (size_t offset = 0; offset < count;) {
        if (count - offset < 2)
            recomp_fail("truncated RDP command", (uint32_t)offset * 4);
        uint32_t w0 = words[offset] | 0xc0000000u, w1 = words[offset + 1];
        unsigned op = w0 >> 24;
        size_t length = 2;
        if (op >= 0xc8 && op <= 0xcf) {
            length = recomp_rdp_triangle_words(w0);
            if (count - offset < length)
                recomp_fail("truncated RDP triangle", (uint32_t)offset * 4);
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
                length = 4;
                if (count - offset < length)
                    recomp_fail("truncated RDP texture rectangle", (uint32_t)offset * 4);
                texture_rectangle(s, w0, w1, words[offset + 2], words[offset + 3], op == 0xe5);
            } else if (!redundant && !consume_rdp(s, w0, w1)) {
                fprintf(stderr, "RDP command %08x %08x at word %zu\n", w0, w1, offset);
                recomp_fail("unsupported RDP command", (uint32_t)offset * 4);
            }
        }
        offset += length;
    }
    draw_end(s, 0);
    s->rt = original;
}
void recomp_rdp_gles2_readback_range(RecompRDPGles2 *s, uint32_t *address, size_t *bytes) {
    owner(s);
    *address = s->full_sync ? s->color_address : 0;
    *bytes = *address ? (size_t)s->width * s->height * (s->color_siz == 3 ? 4u : 2u) : 0;
}
void recomp_rdp_gles2_set_framebuffer(RecompRDPGles2 *s, uint32_t address) {
    owner(s);
    s->present_address = address;
}
void recomp_rdp_gles2_set_framebuffer_sizes(RecompRDPGles2 *s, const RecompFramebufferSize *sizes, size_t count) {
    owner(s);
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
        if (!state.black && s->present_address && s->surfaces[i].valid && s->surfaces[i].address == s->present_address)
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
        if (!height)
            recomp_fail("GLES2 VI width is not in the game's framebuffer profile", width);
    }
    Surface *image = &s->cpu_image;
    if (image->valid && (image->width != width || image->height != height))
        destroy_surface(s, image);
    if (!image->valid)
        create_surface(s, image, width, height);
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
void recomp_rdp_gles2_present(RecompRDPGles2 *s) {
    owner(s);
    flush(s);
    Surface *image = display_image(s);
    s->gl.BindFramebuffer(GL_FRAMEBUFFER, 0);
    int width, height;
    SDL_GL_GetDrawableSize(s->window, &width, &height);
    s->gl.Disable(GL_SCISSOR_TEST);
    s->gl.ClearColor(0, 0, 0, 1);
    s->gl.Clear(GL_COLOR_BUFFER_BIT);
    int w = width, h = s->anamorphic ? height : width * 3 / 4;
    if (!s->anamorphic && h > height) {
        h = height;
        w = height * 4 / 3;
    }
    blit_texture(s, image, 0, (width - w) / 2, (height - h) / 2, w, h, true);
    SDL_GL_SwapWindow(s->window);
    bind_surface(s);
    s->textures_dirty = true;
}
bool recomp_rdp_gles2_poll(RecompRDPGles2 *s) {
    owner(s);
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT)
            return false;
        if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            s->mouse_released = true;
            s->mouse_frame = (RecompMouseFrame){0};
        }
        if (event.type == SDL_MOUSEMOTION && s->mouse_captured) {
            s->mouse_frame.dx += event.motion.xrel;
            s->mouse_frame.dy += event.motion.yrel;
        }
        if (event.type == SDL_MOUSEWHEEL && s->mouse_captured) {
            int64_t delta = event.wheel.y;
            if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED)
                delta = -delta;
            s->mouse_frame.wheel = recomp_mouse_wheel_sum(s->mouse_frame.wheel, delta);
        }
        if (event.type != SDL_KEYDOWN)
            continue;
        SDL_Scancode key = event.key.keysym.scancode;
        if (key == SDL_SCANCODE_F10)
            return false;
        if (!event.key.repeat) {
            if (key == SDL_SCANCODE_ESCAPE) {
                if (!s->input_config.mouse_enabled)
                    return false;
                s->mouse_released = s->mouse_captured;
            }
            if (key == SDL_SCANCODE_F4)
                s->mouse_released = false;
            if (key == SDL_SCANCODE_F7)
                s->expanded_aspect = !s->expanded_aspect;
        }
        if (s->input_config.fov_adjust && (SDL_GetWindowFlags(s->window) & SDL_WINDOW_INPUT_FOCUS) &&
            (key == SDL_SCANCODE_KP_PLUS || key == SDL_SCANCODE_KP_MINUS))
            s->input_config.fov_adjust(key == SDL_SCANCODE_KP_PLUS ? 1 : -1);
    }
    return true;
}
bool recomp_rdp_gles2_input(RecompRDPGles2 *s, RecompPad *pad) {
    owner(s);
    s->mouse_frame = (RecompMouseFrame){0};
    s->camera_stick = (RecompCameraStick){0};
    if (!recomp_rdp_gles2_poll(s))
        return false;
    if (s->input_config.aspect_update) {
        int width, height;
        SDL_GetWindowSize(s->window, &width, &height);
        float aspect = s->expanded_aspect && height > 0 ? fmaxf(4.0f / 3, (float)width / height) : 4.0f / 3;
        s->anamorphic = s->input_config.aspect_update(s->input_config.aspect_userdata, aspect);
    } else
        s->anamorphic = false;
    bool focused = (SDL_GetWindowFlags(s->window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    bool capture = focused && s->input_config.mouse_enabled && s->input_config.mouse_gameplay && !s->mouse_released;
    if (capture != s->mouse_captured) {
        if (SDL_SetRelativeMouseMode(capture ? SDL_TRUE : SDL_FALSE) != 0)
            capture = false;
        SDL_GetRelativeMouseState(NULL, NULL);
        s->mouse_frame = (RecompMouseFrame){0};
    }
    s->mouse_captured = capture;
    s->mouse_frame.active = capture;
    if (!capture)
        s->mouse_frame = (RecompMouseFrame){0};
    *pad = (RecompPad){.connected = true};
    if (!focused)
        return true;
    const uint8_t *key = SDL_GetKeyboardState(NULL);
    bool keyboard_c = s->input_config.keyboard_c_buttons;
    pad->stick_x = (int8_t)((key[keyboard_c ? SDL_SCANCODE_D : SDL_SCANCODE_RIGHT] -
                             key[keyboard_c ? SDL_SCANCODE_A : SDL_SCANCODE_LEFT]) *
                            80);
    pad->stick_y = (int8_t)((key[keyboard_c ? SDL_SCANCODE_W : SDL_SCANCODE_UP] -
                             key[keyboard_c ? SDL_SCANCODE_S : SDL_SCANCODE_DOWN]) *
                            80);
    if (keyboard_c) {
        if (key[SDL_SCANCODE_UP])
            pad->buttons |= 8;
        if (key[SDL_SCANCODE_DOWN])
            pad->buttons |= 4;
        if (key[SDL_SCANCODE_LEFT])
            pad->buttons |= 2;
        if (key[SDL_SCANCODE_RIGHT])
            pad->buttons |= 1;
    }
    if (s->input_config.mouse_enabled) {
        if (key[SDL_SCANCODE_RETURN] || key[SDL_SCANCODE_Q])
            pad->buttons |= 0x8000;
        if (key[SDL_SCANCODE_E] || key[SDL_SCANCODE_R])
            pad->buttons |= 0x4000;
        if (key[SDL_SCANCODE_TAB])
            pad->buttons |= 0x1000;
        if (capture) {
            uint32_t buttons = SDL_GetMouseState(NULL, NULL);
            RecompMouseFrame *m = &s->mouse_frame;
            m->move_x = (int8_t)(key[SDL_SCANCODE_D] - key[SDL_SCANCODE_A]);
            m->move_y = (int8_t)(key[SDL_SCANCODE_W] - key[SDL_SCANCODE_S]);
            if (buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) {
                m->actions |= RECOMP_MOUSE_ATTACK;
                pad->buttons |= 0x2000;
            }
            if (buttons & SDL_BUTTON(SDL_BUTTON_RIGHT)) {
                m->actions |= RECOMP_MOUSE_SECONDARY_ATTACK;
                pad->buttons |= 0x10;
            }
            if (key[SDL_SCANCODE_SPACE])
                m->actions |= RECOMP_MOUSE_JUMP;
            if (key[SDL_SCANCODE_LSHIFT] || key[SDL_SCANCODE_RSHIFT])
                m->actions |= RECOMP_MOUSE_SPEED;
            if (key[SDL_SCANCODE_Q])
                m->actions |= RECOMP_MOUSE_NEXT_WEAPON;
            if (key[SDL_SCANCODE_R])
                m->actions |= RECOMP_MOUSE_PREVIOUS_WEAPON;
            if (key[SDL_SCANCODE_E])
                m->actions |= RECOMP_MOUSE_USE;
            if (key[SDL_SCANCODE_LCTRL] || key[SDL_SCANCODE_RCTRL])
                m->actions |= RECOMP_MOUSE_DEFEND;
            if (key[SDL_SCANCODE_F])
                m->actions |= RECOMP_MOUSE_STATUS;
        }
        recomp_input_poll(s->input, pad, s->input_config.gamepad_camera ? &s->camera_stick : NULL);
        return true;
    }
    const struct {
        SDL_Scancode key;
        uint16_t button;
    } map[] = {{SDL_SCANCODE_A, 0x8000}, {SDL_SCANCODE_RETURN, 0x8000}, {SDL_SCANCODE_X, 0x4000},
               {SDL_SCANCODE_Z, 0x2000}, {SDL_SCANCODE_S, 0x1000},      {SDL_SCANCODE_C, 0x20},
               {SDL_SCANCODE_V, 0x10},   {SDL_SCANCODE_I, 8},           {SDL_SCANCODE_K, 4},
               {SDL_SCANCODE_J, 2},      {SDL_SCANCODE_L, 1},           {SDL_SCANCODE_T, 0x800},
               {SDL_SCANCODE_G, 0x400},  {SDL_SCANCODE_F, 0x200},       {SDL_SCANCODE_H, 0x100}};
    for (unsigned i = 0; i < sizeof(map) / sizeof(*map); i++)
        if (key[map[i].key])
            pad->buttons |= map[i].button;
    if (pad->stick_x && pad->stick_y) {
        pad->stick_x = (int8_t)(pad->stick_x * 7 / 10);
        pad->stick_y = (int8_t)(pad->stick_y * 7 / 10);
    }
    recomp_input_poll(s->input, pad, s->input_config.gamepad_camera ? &s->camera_stick : NULL);
    return true;
}
void recomp_rdp_gles2_set_input_config(RecompRDPGles2 *s, const RecompGles2InputConfig *config) {
    owner(s);
    s->input_config = *config;
}
RecompMouseFrame recomp_rdp_gles2_mouse_frame(RecompRDPGles2 *s) {
    owner(s);
    RecompMouseFrame result = s->mouse_frame;
    s->mouse_frame.dx = s->mouse_frame.dy = 0;
    s->mouse_frame.wheel = 0;
    return result;
}
RecompCameraStick recomp_rdp_gles2_camera_stick(RecompRDPGles2 *s) {
    owner(s);
    return s->camera_stick;
}
bool recomp_rdp_gles2_full_sync(RecompRDPGles2 *s) {
    return s->full_sync;
}
uint64_t recomp_rdp_gles2_triangles(RecompRDPGles2 *s) {
    return s->triangles;
}
bool recomp_rdp_gles2_capture(RecompRDPGles2 *s, const char *path) {
    owner(s);
    flush(s);
    Surface *f = display_image(s);
    unsigned width = f->width, height = f->height;
    if (s->color_gamma != 1.0f || s->antialias) {
        if (s->capture.valid && (s->capture.width != width || s->capture.height != height))
            destroy_surface(s, &s->capture);
        if (!s->capture.valid)
            create_surface(s, &s->capture, width, height);
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
    bind_surface(s);
    return ok;
}
void recomp_rdp_gles2_destroy(RecompRDPGles2 *s) {
    if (!s)
        return;
    if (s->context) {
        owner(s);
        destroy_surface(s, &s->cpu_image);
        if (s->mouse_captured)
            SDL_SetRelativeMouseMode(SDL_FALSE);
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
            }
        for (unsigned i = 0; i < 16; i++)
            if (s->depths[i].buffer)
                s->gl.DeleteRenderbuffers(1, &s->depths[i].buffer);
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
    free(s);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
}
