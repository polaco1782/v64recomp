#include "internal.h"
#include "screen_space.h"
#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#endif
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void recomp_gles2_require_owner(RecompRDPGles2 *s) {
#ifdef __EMSCRIPTEN__
    if (SDL_ThreadID() != s->owner || emscripten_webgl_get_current_context() != s->context)
#else
    if (SDL_ThreadID() != s->owner || SDL_GL_GetCurrentContext() != s->context)
#endif
        recomp_fail("GLES2 requires its owner thread/context", 0);
}
static bool quality_option(const char *name, bool fallback) {
    const char *value = getenv(name);
    if (!value || !*value)
        return fallback;
    if (strcmp(value, "0") && strcmp(value, "1"))
        recomp_fail("GLES2 quality switches accept 0 or 1", 0);
    return value[0] == '1';
}

RecompRDPGles2 *recomp_rdp_gles2_create(Runtime *rt, const char *title) {
    SDL_SetMainReady();
    if (!rt || !rt->rdram || SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
        return NULL;
#ifndef __EMSCRIPTEN__
    SDL_GL_ResetAttributes();
#ifdef __linux__
    SDL_SetHint(SDL_HINT_VIDEO_X11_FORCE_EGL, "1");
#elif defined(_WIN32)
    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
#endif
    if (SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES) ||
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3) || SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0) ||
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1))
        return NULL;
#endif
    RecompRDPGles2 *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    s->rt = rt;
    s->draw_ram = rt->rdram;
    static const RecompFramebufferSize default_sizes[] = {{320, 240}};
    s->framebuffer_sizes = default_sizes;
    s->framebuffer_size_count = 1;
    s->width = 320;
    s->height = 240;
    s->color_siz = 2;
    s->owner = SDL_ThreadID();
    const char *expanded = getenv("RECOMP_EXPANDED_ASPECT");
    s->expanded_aspect = expanded && !strcmp(expanded, "1");
#if defined(__aarch64__) || defined(_M_ARM64) || defined(__EMSCRIPTEN__)
    s->render_scale = 1;
    s->antialias = false;
#else
    s->render_scale = 4;
    s->antialias = true;
#endif
    s->antialias = quality_option("RECOMP_GLES2_AA", s->antialias);
    s->show_reflections = quality_option("RECOMP_GLES2_SSR", false);
    s->show_occlusion = quality_option("RECOMP_GLES2_SSAO", false);
    s->show_torch_shadows = quality_option("RECOMP_GLES2_TORCH_SHADOWS", false);
    s->show_torches = s->torch_mode = quality_option("RECOMP_GLES2_TORCH_LIGHTS", false) || s->show_torch_shadows;
    s->level_reflections = quality_option("RECOMP_GLES2_RT_REFLECTIONS", false);
    s->screen_effects = s->level_reflections || s->torch_mode || s->show_occlusion || s->show_reflections;
    s->hq2x = quality_option("RECOMP_GLES2_HQ2X", true);
    s->mipmaps = quality_option("RECOMP_GLES2_MIPMAPS", true);
    const char *scale = getenv("RECOMP_GLES2_SCALE");
    if (scale && *scale) {
        char *end;
        unsigned long value = strtoul(scale, &end, 10);
        if (*end || value < 1 || value > 4)
            recomp_fail("RECOMP_GLES2_SCALE must be 1..4", 0);
        s->render_scale = (unsigned)value;
    }
    s->color_gamma = 1.0f;
    const char *gamma = getenv("RECOMP_GLES2_GAMMA");
    if (gamma && *gamma) {
        char *end;
        float value = strtof(gamma, &end);
        if (*end || !isfinite(value) || value < 0.5f || value > 2.5f)
            recomp_fail("RECOMP_GLES2_GAMMA must be 0.5..2.5", 0);
        s->color_gamma = value;
        s->gamma_overridden = true;
    }
#ifdef __EMSCRIPTEN__
    /* SDL's EGL path does not establish WebGL's worker-local context handle.
     * Create it through html5.h so asynchronous buffer/texture uploads target
     * the correct owner, and present only complete frames. SDL owns input. */
    s->window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 960, 720, SDL_WINDOW_RESIZABLE);
    EmscriptenWebGLContextAttributes attributes;
    emscripten_webgl_init_context_attributes(&attributes);
    attributes.alpha = false;
    attributes.depth = false;
    attributes.stencil = false;
    attributes.antialias = false;
    attributes.majorVersion = 2;
    attributes.explicitSwapControl = true;
    attributes.proxyContextToMainThread = EMSCRIPTEN_WEBGL_CONTEXT_PROXY_ALWAYS;
    if (s->window) {
        s->context = emscripten_webgl_create_context("#canvas", &attributes);
        if (s->context > 0 && emscripten_webgl_make_context_current(s->context) != EMSCRIPTEN_RESULT_SUCCESS) {
            emscripten_webgl_destroy_context(s->context);
            s->context = 0;
        }
    }
    if (s->context < 0)
        s->context = 0;
#else
    s->window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 960,
                                 SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (s->window)
        s->context = SDL_GL_CreateContext(s->window);
#endif
    if (!s->context) {
#ifdef __EMSCRIPTEN__
        fprintf(stderr, "WebGL 2 is required: could not create a context\n");
#else
        fprintf(stderr, "OpenGL ES 3.0 or later is required: %s\n", SDL_GetError());
#endif
        recomp_rdp_gles2_destroy(s);
        return NULL;
    }
#ifdef __EMSCRIPTEN__
#define LOAD(name, type) s->gl.name = gl##name;
#else
#define LOAD(name, type)                                                                                               \
    {                                                                                                                  \
        void *entry = SDL_GL_GetProcAddress("gl" #name);                                                               \
        _Static_assert(sizeof(entry) == sizeof(s->gl.name), "GL pointer size");                                        \
        if (!entry)                                                                                                    \
            recomp_fail("missing GLES2 gl" #name, 0);                                                                  \
        memcpy(&s->gl.name, &entry, sizeof(entry));                                                                    \
    }
#endif
    GL_FUNCTIONS(LOAD)
#undef LOAD
#ifdef RECOMP_GLES2_TEXTURE_PACKS
    GLint max_texture_size = 0;
    s->gl.GetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
    s->max_texture_size = max_texture_size > 0 ? (unsigned)max_texture_size : 0;
#endif
#ifndef __EMSCRIPTEN__
    const char *version = (const char *)s->gl.GetString(GL_VERSION);
    unsigned major = 0;
    if (!version || sscanf(version, "OpenGL ES %u", &major) != 1 || major < 3) {
        fprintf(stderr, "OpenGL ES 3.0 or later is required; driver reported %s\n", version ? version : "no version");
        recomp_rdp_gles2_destroy(s);
        return NULL;
    }
#endif
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
    recomp_gles2_bind_surface(s);
    const char *vs =
        "#version 300 es\nin vec4 position;in vec2 uv;out vec2 tc;void main(){gl_Position=position;tc=uv;}";
    const char *fs = "#version 300 es\nprecision highp float;\n"
                     "in vec2 tc;out vec4 outputColor;uniform sampler2D image;uniform float colorGamma,aa;"
                     "uniform vec2 pixel;vec3 shadeScene(vec3 base);"
                     "void main(){vec4 c=texture(image,tc);if(aa>0.0){"
                     "vec3 nw=texture(image,tc+pixel*vec2(-1.0,-1.0)).rgb;"
                     "vec3 ne=texture(image,tc+pixel*vec2(1.0,-1.0)).rgb;"
                     "vec3 sw=texture(image,tc+pixel*vec2(-1.0,1.0)).rgb;"
                     "vec3 se=texture(image,tc+pixel*vec2(1.0,1.0)).rgb;vec3 l=vec3(0.299,0.587,0.114);"
                     "float lnw=dot(nw,l),lne=dot(ne,l),lsw=dot(sw,l),lse=dot(se,l),lm=dot(c.rgb,l);"
                     "float lo=min(lm,min(min(lnw,lne),min(lsw,lse))),hi=max(lm,max(max(lnw,lne),max(lsw,lse)));"
                     "vec2 d=vec2(-((lnw+lne)-(lsw+lse)),(lnw+lsw)-(lne+lse));"
                     "float reduce=max((lnw+lne+lsw+lse)*0.03125,0.0078125);"
                     "d=clamp(d/(min(abs(d.x),abs(d.y))+reduce),vec2(-8.0),vec2(8.0))*pixel;"
                     "vec3 a=0.5*(texture(image,tc-d/6.0).rgb+texture(image,tc+d/6.0).rgb);"
                     "vec3 b=a*0.5+0.25*(texture(image,tc-d*0.5).rgb+texture(image,tc+d*0.5).rgb);"
                     "float lb=dot(b,l);c.rgb=(lb<lo||lb>hi)?a:b;}"
                     "c.rgb=shadeScene(c.rgb);if(colorGamma!=1.0)c.rgb=pow(c.rgb,vec3(colorGamma));outputColor=c;}";
    const char *parts[] = {fs, s->screen_effects ? screen_space_shader : "vec3 shadeScene(vec3 base){return base;}"};
    GLuint v = recomp_gles2_shader(s, GL_VERTEX_SHADER, vs),
           f = recomp_gles2_shader_parts(s, GL_FRAGMENT_SHADER, parts, 2);
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
    if (s->screen_effects) {
        s->gl.UseProgram(s->blit);
        s->gl.Uniform1i(s->gl.GetUniformLocation(s->blit, "sceneDepth"), 1);
        s->blit_level_reflections = s->gl.GetUniformLocation(s->blit, "levelReflections");
        s->gl.Uniform1i(s->gl.GetUniformLocation(s->blit, "levelImage"), 2);
        s->blit_reflections = s->gl.GetUniformLocation(s->blit, "showReflections");
        s->blit_torch_shadows = s->gl.GetUniformLocation(s->blit, "showTorchShadows");
        s->blit_player_min_depth = s->gl.GetUniformLocation(s->blit, "playerMinDepth");
        s->blit_capsule_count = s->gl.GetUniformLocation(s->blit, "capsuleCount");
        s->blit_capsule_start = s->gl.GetUniformLocation(s->blit, "capsuleStart[0]");
        s->blit_capsule_end = s->gl.GetUniformLocation(s->blit, "capsuleEnd[0]");
        s->blit_camera = s->gl.GetUniformLocation(s->blit, "torchCamera");
        s->blit_count = s->gl.GetUniformLocation(s->blit, "torchCount");
        s->blit_centers = s->gl.GetUniformLocation(s->blit, "torchCenters[0]");
        s->blit_colors = s->gl.GetUniformLocation(s->blit, "torchColors[0]");
        s->blit_occlusion = s->gl.GetUniformLocation(s->blit, "showOcclusion");
    }
#ifdef RECOMP_DEBUG
    fprintf(stderr, "%s: scale=%u, FXAA=%s, mipmaps=%s, HQ2x=%s\n", s->gl.GetString(GL_VERSION), s->render_scale,
            s->antialias ? "on" : "off", s->mipmaps ? "on" : "off", s->hq2x ? "on" : "off");
#endif
    s->input = recomp_input_create();
    if (!s->input)
        recomp_fail("GLES2 controller initialization failed", 0);
#ifndef __EMSCRIPTEN__
    (void)SDL_GL_SetSwapInterval(0);
#endif
    const char *fps = getenv("RECOMP_GLES2_FPS");
    if (fps && *fps) {
        if (!strcmp(fps, "60"))
            recomp_gles2_init_interpolation(s);
        else if (strcmp(fps, "30") && strcmp(fps, "native"))
            recomp_fail("RECOMP_GLES2_FPS accepts native, 30 or 60", 0);
    }
    return s;
}

#ifdef RECOMP_GLES2_CHEATS
void recomp_rdp_gles2_enable_cheats(RecompRDPGles2 *s, const char *data_path) {
    recomp_gles2_require_owner(s);
    if (!s->cheats && s->rt->rom && s->rt->rom_size >= 64)
        s->cheats = gles2_cheats_create(s->rt, s->window, data_path ? data_path : ".");
}
#endif

void recomp_rdp_gles2_destroy(RecompRDPGles2 *s) {
    if (!s)
        return;
    recomp_gles2_queue_destroy(s);
#ifdef RECOMP_GLES2_TEXTURE_PACKS
    gles2_editor_destroy(s->editor);
    gles2_texture_pack_close(s->texture_pack);
    free(s->texture_overrides);
#endif
    if (s->context) {
        recomp_gles2_require_owner(s);
        recomp_gles2_destroy_interpolation(s);
        recomp_gles2_clear_texture_caches(s);
#ifdef RECOMP_GLES2_CHEATS
        gles2_cheats_destroy(s->cheats);
#endif
        recomp_gles2_destroy_surface(s, &s->cpu_image);
        if (s->mouse_captured)
            SDL_SetRelativeMouseMode(SDL_FALSE);
        if (s->input)
            recomp_input_destroy(s->input);
        if (s->buffer)
            s->gl.DeleteBuffers(1, &s->buffer);
        if (s->white)
            s->gl.DeleteTextures(1, &s->white);
        recomp_gles2_destroy_wireframe(s);
        if (s->blit)
            s->gl.DeleteProgram(s->blit);
        for (unsigned i = 0; i < s->program_count; i++)
            s->gl.DeleteProgram(s->programs[i].id);
        for (unsigned i = 0; i < 8; i++)
            recomp_gles2_destroy_surface(s, &s->surfaces[i]);
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
#ifdef __EMSCRIPTEN__
        emscripten_webgl_destroy_context(s->context);
#else
        SDL_GL_DeleteContext(s->context);
#endif
    }
    if (s->window)
        SDL_DestroyWindow(s->window);
    free(s);
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
}
