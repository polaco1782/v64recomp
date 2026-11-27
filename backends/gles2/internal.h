#ifndef RECOMP_GLES2_INTERNAL_H
#define RECOMP_GLES2_INTERNAL_H

#ifdef RECOMP_GLES2_CHEATS
#include "cheat_overlay.h"
#endif
#include "input.h"
#include "rdp_gles2.h"
#include "tile.h"
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

/* SDL supplies ES2 headers on all targets. These entry points are core in ES3. */
#ifndef GL_COLOR
#define GL_COLOR 0x1800
#endif
typedef void(GL_APIENTRYP ClearBufferfvProc)(GLenum, GLint, const GLfloat *);

typedef void(GL_APIENTRYP ReadBufferProc)(GLenum);
typedef void(GL_APIENTRYP BlitFramebufferProc)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield,
                                               GLenum);

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
    X(DrawBuffers, PFNGLDRAWBUFFERSEXTPROC)                                                                            \
    X(ClearBufferfv, ClearBufferfvProc)                                                                                \
    X(BlitFramebuffer, BlitFramebufferProc)                                                                            \
    X(ReadBuffer, ReadBufferProc)                                                                                      \
    X(Finish, PFNGLFINISHPROC)

enum { RECOMP_GLES2_BATCH_VERTICES = 1536 };

typedef struct Vertex {
    float position[4], color[4], uv[2], fog, reciprocal;
#ifdef RECOMP_GLES2_TEXTURE_PACKS
    float pick_offset;
#endif
} Vertex;
typedef struct Program {
    uint32_t mux0, mux1, cycle;
    GLuint id;
    GLint prim, env, fog, lod, alpha, coverage_alpha, fogen, perspective, uv_bounds;
    GLint tile[2][3], record_depth, blended;
#ifdef RECOMP_GLES2_TEXTURE_PACKS
    GLint pick_base;
#endif
    bool texture[2];
} Program;
enum { GLES2_TEXTURE_SLOTS = 1024, GLES2_NATIVE_WAYS = 16, GLES2_REPLACEMENT_WAYS = 4 };
typedef struct Texture {
    uint64_t hash, used;
    GLuint id;
    unsigned hq2x_wrap;
    size_t bytes;
#ifdef RECOMP_GLES2_TEXTURE_PACKS
    Gles2EditorPreview *preview;
    unsigned image_width, image_height;
    uint64_t rice_texels, rice_palette;
    size_t unidentified;
    bool retry_rice;
#endif
} Texture;
typedef struct TextureCache {
    Texture slots[GLES2_TEXTURE_SLOTS];
    size_t bytes;
} TextureCache;
typedef struct Surface {
    uint32_t address;
    unsigned width, height;
    GLuint framebuffer, texture, depth, scene_depth, reflection_texture;
#ifdef RECOMP_GLES2_TEXTURE_PACKS
    GLuint pick_texture;
#endif
    uint64_t reflection_serial;
    RecompTorchLights lights;
    uint64_t light_serial;
    uint64_t serial;
    bool valid, has_midpoint, clear_before_wireframe;
} Surface;
typedef struct DepthSurface {
    uint32_t address;
    unsigned width, height;
    GLuint buffer;
    bool clear;
} DepthSurface;
struct RecompRDPGles2 {
    Runtime *rt;
    uint8_t *draw_ram;
    struct RenderQueue *queue;
    SDL_Window *window;
#ifdef RECOMP_GLES2_CHEATS
    Gles2Cheats *cheats;
#endif
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
    bool menu_confirm_down, menu_confirm_blocked;
    float presentation_aspect;
    struct {
#define DECLARE(name, type) type name;
        GL_FUNCTIONS(DECLARE)
#undef DECLARE
    } gl;
    GLuint buffer, white, blit;
    GLint blit_gamma, blit_pixel, blit_aa;
    bool torch_mode, show_torches, show_torch_shadows;
    GLint blit_capsule_count, blit_capsule_start, blit_capsule_end, blit_player_min_depth;
    GLint blit_camera, blit_count, blit_centers, blit_colors, blit_torch_shadows;
    bool level_reflections;
    RecompReflectionSource reflection_source;
    const RecompLevelReflection *task_reflection;
    GLint blit_level_reflections;
    RecompTorchLights task_lights;
    uint64_t light_serial;
    RecompTorchSource torch_source;
    GLint blit_reflections, blit_occlusion;
    bool antialias, mipmaps, hq2x, screen_effects, show_reflections, show_occlusion;
    Program programs[256];
    unsigned program_count;
    TextureCache textures;
    uint64_t texture_clock;
    GLuint active_texture[2];
    unsigned tex_width[2], tex_height[2];
#ifdef RECOMP_GLES2_TEXTURE_PACKS
    Gles2TexturePack *texture_pack;
    TextureCache replacements;
    /* Low nine bits identify the load's starting TMEM word. */
    uint64_t rice_load;
    Gles2RiceWord rice_words[512];
    bool rice_active;
    Gles2TextureEditor *editor;
    struct Gles2Picker *picker;
    uint64_t active_tile_key[2];
    char *texture_overrides, *texture_zip;
    unsigned max_texture_size;
    bool original_textures;
#endif
    bool textures_dirty;
    Surface surfaces[8], midpoints[8];
    struct Interpolation *interpolation;
    Surface *last_display;
    DepthSurface depths[16];
    Surface readback, capture, cpu_image;
    Surface *surface;
    unsigned render_scale;
    bool ram_scanout;
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
};
GLuint recomp_gles2_shader(RecompRDPGles2 *, GLenum type, const char *source);
GLuint recomp_gles2_shader_parts(RecompRDPGles2 *, GLenum, const char *const *, GLsizei);
Program *recomp_gles2_combiner(RecompRDPGles2 *);
void recomp_gles2_draw_wireframe(RecompRDPGles2 *, const Program *);
void recomp_gles2_toggle_wireframe(RecompRDPGles2 *);
void recomp_gles2_begin_wireframe_draw(RecompRDPGles2 *);
void recomp_gles2_destroy_wireframe(RecompRDPGles2 *);

void recomp_gles2_require_owner(RecompRDPGles2 *);
void recomp_gles2_drawable_size(RecompRDPGles2 *, int *width, int *height);
void recomp_gles2_flush(RecompRDPGles2 *);
void recomp_gles2_init_interpolation(RecompRDPGles2 *);
void recomp_gles2_destroy_interpolation(RecompRDPGles2 *);
void recomp_gles2_begin_interpolation(RecompRDPGles2 *);
void recomp_gles2_end_interpolation(RecompRDPGles2 *);
Surface *recomp_gles2_midpoint_target(RecompRDPGles2 *);
Vertex *recomp_gles2_midpoint_triangle(RecompRDPGles2 *, uint32_t flags, unsigned start, float decal_bias);
const Vertex *recomp_gles2_midpoint_batch(RecompRDPGles2 *);
Surface *recomp_gles2_interpolated_display(RecompRDPGles2 *, Surface *);
void recomp_gles2_store_reflection(RecompRDPGles2 *, Surface *);
void recomp_gles2_bind_scene_depth(RecompRDPGles2 *, Surface *);
void recomp_gles2_readback_color(RecompRDPGles2 *);
void recomp_gles2_upload_i8(RecompRDPGles2 *, unsigned x0, unsigned y0, unsigned x1, unsigned y1, bool fill);
void recomp_gles2_bind_surface(RecompRDPGles2 *);
DepthSurface *recomp_gles2_depth_surface(RecompRDPGles2 *, unsigned width, unsigned height);
void recomp_gles2_create_surface(RecompRDPGles2 *, Surface *, unsigned width, unsigned height);
void recomp_gles2_destroy_surface(RecompRDPGles2 *, Surface *);
void recomp_gles2_clear_texture_caches(RecompRDPGles2 *);
GLuint recomp_gles2_texture(RecompRDPGles2 *, Tile *, unsigned unit);
void recomp_gles2_load_tile(RecompRDPGles2 *, Tile *, uint32_t w0, uint32_t w1, bool block, bool palette);
#ifdef RECOMP_GLES2_TEXTURE_PACKS
void recomp_gles2_reload_textures(RecompRDPGles2 *);
void recomp_gles2_pick_attach(RecompRDPGles2 *, Surface *);
void recomp_gles2_pick_clear(RecompRDPGles2 *, const Surface *);
void recomp_gles2_pick_batch(RecompRDPGles2 *, const Program *);
void recomp_gles2_pick_destroy(RecompRDPGles2 *);
void recomp_gles2_pick_outline(RecompRDPGles2 *, const Surface *, unsigned hovered);
void recomp_gles2_editor_display(RecompRDPGles2 *, bool outlines, unsigned hovered);
unsigned recomp_gles2_editor_hit(RecompRDPGles2 *, int x, int y);
unsigned recomp_gles2_pick_keys(RecompRDPGles2 *, unsigned id, uint64_t keys[2]);
#endif

TextureTransfer recomp_gles2_texture_transfer(unsigned image_width, unsigned image_siz, unsigned tile_siz, uint32_t w0,
                                              uint32_t w1);
size_t recomp_gles2_command_words(uint32_t w0, size_t available);
void recomp_gles2_record_color(RecompRDPGles2 *, uint32_t address, size_t bytes);
void recomp_gles2_queue_destroy(RecompRDPGles2 *);

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
