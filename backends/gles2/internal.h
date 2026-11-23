#ifndef RECOMP_GLES2_INTERNAL_H
#define RECOMP_GLES2_INTERNAL_H

#include "input.h"
#include "rdp_gles2.h"
#ifdef RECOMP_GLES2_TEXTURE_PACKS
#include "editor.h"
#include "texture_pack.h"
#endif
#include <SDL.h>
#include <string.h>
#include <SDL_opengles2.h>
#ifdef __EMSCRIPTEN__
#include <emscripten/html5.h>
#endif

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
    X(DrawElements, PFNGLDRAWELEMENTSPROC)                                                                             \
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
    X(GetIntegerv, PFNGLGETINTEGERVPROC)                                                                               \
    X(GetString, PFNGLGETSTRINGPROC)                                                                                   \
    X(ReadPixels, PFNGLREADPIXELSPROC)                                                                                 \
    X(PixelStorei, PFNGLPIXELSTOREIPROC)                                                                               \
    X(GenTextures, PFNGLGENTEXTURESPROC)                                                                               \
    X(BindTexture, PFNGLBINDTEXTUREPROC)                                                                               \
    X(TexImage2D, PFNGLTEXIMAGE2DPROC)                                                                                 \
    X(TexSubImage2D, PFNGLTEXSUBIMAGE2DPROC)                                                                           \
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

enum { RECOMP_GLES2_BATCH_VERTICES = 1536 };

typedef struct Vertex {
    float position[4], color[4], uv[2], fog, reciprocal;
} Vertex;
typedef struct Tile {
    unsigned fmt, siz, line, tmem, palette, cs, ct, ms, mt, ss, st, sl, tl, sh, th;
} Tile;
typedef struct Program {
    uint32_t mux0, mux1, cycle;
    GLuint id;
    GLint prim, env, fog, lod, alpha, coverage_alpha, fogen, perspective;
    GLint tile[2][3];
    bool texture[2];
} Program;
typedef struct Texture {
    uint64_t hash, used;
    GLuint id;
#ifdef RECOMP_GLES2_TEXTURE_PACKS
    uint64_t rt64_id;
    size_t replacement_bytes;
    bool has_rt64_id;
#endif
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
    uint8_t *draw_ram; /* Worker snapshot during a task, live RDRAM otherwise. */
    SDL_Window *window;
#ifdef __EMSCRIPTEN__
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE context;
#else
    SDL_GLContext context;
#endif
    SDL_threadID owner;
    RecompInput *input;
    RecompGles2InputConfig input_config;
    RecompMouseFrame mouse_frame;
    RecompCameraStick camera_stick;
    bool mouse_captured, mouse_released, expanded_aspect;
    float presentation_aspect;
    struct {
#define DECLARE(name, type) type name;
        GL_FUNCTIONS(DECLARE)
#undef DECLARE
    } gl;
    GLuint buffer, white, blit;
    GLint blit_gamma, blit_pixel, blit_aa;
    bool antialias, mipmaps;
    Program programs[256];
    unsigned program_count;
    Texture textures[128];
    uint64_t texture_clock;
    GLuint active_texture[2];
    unsigned tex_width[2], tex_height[2];
#ifdef RECOMP_GLES2_TEXTURE_PACKS
    Gles2TexturePack *texture_pack;
    Gles2TextureEditor *editor;
    char *texture_overrides;
    size_t replacement_bytes;
    unsigned max_texture_size;
    uint64_t texture_id[2];
    bool texture_id_valid[2], original_textures;
#endif
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
    bool gamma_overridden;
    uint32_t color_address, depth_address, present_address;
    bool clearing_depth, color_image_set, color_dirty, rdp_perspective;
    GLuint wireframe_program, wireframe_indices;
    GLint wireframe_color;
    uint32_t tmem_source[512]; /* SETTIMG address plus one; zero means unloaded. */
    bool wireframe, drawing_rectangle;
    Vertex batch[RECOMP_GLES2_BATCH_VERTICES];
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
GLuint recomp_gles2_shader(RecompRDPGles2 *, GLenum type, const char *source);
Program *recomp_gles2_combiner(RecompRDPGles2 *);
void recomp_gles2_draw_wireframe(RecompRDPGles2 *, const Program *);
void recomp_gles2_destroy_wireframe(RecompRDPGles2 *);

void recomp_gles2_require_owner(RecompRDPGles2 *);
void recomp_gles2_drawable_size(RecompRDPGles2 *, int *width, int *height);
void recomp_gles2_flush(RecompRDPGles2 *);
void recomp_gles2_readback_color(RecompRDPGles2 *);
void recomp_gles2_upload_i8(RecompRDPGles2 *, unsigned x0, unsigned y0, unsigned x1, unsigned y1, bool fill);
void recomp_gles2_bind_surface(RecompRDPGles2 *);
DepthSurface *recomp_gles2_depth_surface(RecompRDPGles2 *, unsigned width, unsigned height);
void recomp_gles2_create_surface(RecompRDPGles2 *, Surface *, unsigned width, unsigned height);
void recomp_gles2_destroy_surface(RecompRDPGles2 *, Surface *);
GLuint recomp_gles2_texture(RecompRDPGles2 *, Tile *, unsigned unit);
void recomp_gles2_load_tile(RecompRDPGles2 *, Tile *, uint32_t w0, uint32_t w1, bool block, bool palette);
#ifdef RECOMP_GLES2_TEXTURE_PACKS
void recomp_gles2_reload_textures(RecompRDPGles2 *);
#endif

static inline uint32_t span(RecompRDPGles2 *s, uint32_t a, size_t n) {
    return recomp_ram_address(s->rt, 0x80000000u | (a & 0x03ffffff), n);
}
static inline uint32_t load(RecompRDPGles2 *s, uint32_t a, unsigned n) {
    /* Callers have already resolved and checked the complete physical span.
     * Graphics RAM is not a CPU/device access; avoid the generic bus decoder
     * for every texture byte and vertex component. */
#if RECOMP_LITTLE_ENDIAN_HOST
    if (n == 1)
        return s->draw_ram[a ^ 3];
    if (a & 1)
        recomp_fail("unaligned RDP halfword", a);
    uint16_t value;
    memcpy(&value, s->draw_ram + (a ^ 2), sizeof(value));
    return value;
#else
    return n == 1 ? s->draw_ram[a] : (uint32_t)s->draw_ram[a] << 8 | s->draw_ram[a + 1];
#endif
}

static inline void pixel16(unsigned bits, uint8_t *p, bool ia) {
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

#endif
