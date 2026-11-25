#include "internal.h"
#include <stdio.h>

GLuint recomp_gles2_shader(RecompRDPGles2 *s, GLenum type, const char *text) {
    return recomp_gles2_shader_parts(s, type, &text, 1);
}
GLuint recomp_gles2_shader_parts(RecompRDPGles2 *s, GLenum type, const char *const *parts, GLsizei count) {
    GLuint id = s->gl.CreateShader(type);
    s->gl.ShaderSource(id, count, parts, NULL);
    s->gl.CompileShader(id);
    GLint ok;
    s->gl.GetShaderiv(id, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        s->gl.GetShaderInfoLog(id, sizeof(log), NULL, log);
        fprintf(stderr, "%s\n", log);
        for (int i = 0; i < count; i++)
            fprintf(stderr, "%s\n", parts[i]);
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
Program *recomp_gles2_combiner(RecompRDPGles2 *s) {
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
    const char *pick_vertex = "", *pick_position = "", *pick_fragment = "", *pick_output = "";
#ifdef RECOMP_GLES2_TEXTURE_PACKS
    if (s->editor) {
        pick_vertex = "in highp float pickOffset;uniform highp int pickBase;flat out highp int triangle;";
        pick_position = "triangle=pickBase+int(pickOffset);";
        pick_fragment = "flat in highp int triangle;layout(location=2) out highp vec4 pickColor;";
        /* Binary coverage for the ID attachment. With blending enabled a fully
         * transparent fragment preserves the previous triangle under it. */
        pick_output = "pickColor=vec4(vec3(ivec3(triangle,triangle>>8,triangle>>16)&255)/255.0,"
                      "combined.a>0.0 ? 1.0 : 0.0);";
    }
#endif
    const char *vs[] = {"#version 300 es\n", pick_vertex,
                        "in vec4 position;in vec4 color;in vec2 uv;in float fogv;in float reciprocal;"
                        "out vec4 shade;out vec2 texcoord;out float fogfactor;out float invw;"
                        "void main(){gl_Position=position;shade=color;texcoord=uv;fogfactor=fogv;invw=reciprocal;",
                        pick_position, "}"};
    char fs[8192];
    int pos = snprintf(
        fs, sizeof(fs),
        "#version 300 es\nprecision highp float;precision highp int;layout(location=0) out vec4 outputColor;%s%s"
        "in vec4 shade;in vec2 texcoord;in float fogfactor;in float invw;uniform vec4 "
        "prim,env,fogc;uniform "
        "float lod,alphatest,coverageAlpha,fogen,rdpPerspective;"
        "uniform sampler2D tex0,tex1;uniform vec4 dim0,dim1,origin0,origin1,wrap0,wrap1,uvBounds;"
        "vec2 coord(vec2 uv,vec4 d,vec4 o,vec4 w){vec2 p=uv*d.zw-o.xy;"
        "if(o.z<0.5){if(w.x>0.0){p.x=mod(p.x,w.x*(w.z>0.0?2.0:1.0));"
        "if(w.z>0.0)p.x=min(p.x,w.x*2.0-1.0-p.x);}p.x=clamp(p.x,0.0,d.x-1.0);}"
        "if(o.w<0.5){if(w.y>0.0){p.y=mod(p.y,w.y*(w.w>0.0?2.0:1.0));"
        "if(w.w>0.0)p.y=min(p.y,w.y*2.0-1.0-p.y);}p.y=clamp(p.y,0.0,d.y-1.0);}"
        "return (p+vec2(0.5))/d.xy;}"
        "void main(){vec2 sourceUV=texcoord;"
        "if(rdpPerspective>0.5){sourceUV=invw<=0.0?vec2(1023.96875):"
        "clamp(texcoord/invw*16384.0,vec2(-1024.0),vec2(1023.96875));}"
        "vec2 dx=dFdx(sourceUV),dy=dFdy(sourceUV);"
        /* At higher render scales, keep edge fragments within the native rectangle samples. */
        "sourceUV=clamp(sourceUV,uvBounds.xy,uvBounds.zw);"
        "vec4 t0=textureGrad(tex0,coord(sourceUV,dim0,origin0,wrap0),dx*dim0.zw/dim0.xy,dy*dim0.zw/dim0.xy);"
        "vec4 t1=textureGrad(tex1,coord(sourceUV,dim1,origin1,wrap1),dx*dim1.zw/dim1.xy,dy*dim1.zw/dim1.xy);"
        "float noise=fract(sin(dot(gl_FragCoord.xy,vec2(12.9898,78.233)))*43758.5);vec4 combined=vec4(0.0);",
        pick_fragment,
        s->screen_effects ? "layout(location=1) out vec4 sceneDepth;uniform bool recordDepth,blended;" : "");
    if (cycle == 2) {
        pos += snprintf(fs + pos, sizeof(fs) - (size_t)pos, "combined=t0;");
        p->texture[0] = true;
    }
    for (unsigned c = cycle == 1 ? 0 : 1; c < 2 && cycle != 2; c++) {
        bool second_cycle = cycle == 1 && c == 1;
        if (second_cycle)
            pos += snprintf(fs + pos, sizeof(fs) - (size_t)pos,
                            "float compareAlpha=combined.a;vec4 previousTexel=t0;t0=t1;t1=previousTexel;");
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
             "if(%s<alphatest||combined.a<coverageAlpha)discard;"
             "combined.rgb=mix(combined.rgb,fogc.rgb,clamp(fogfactor*fogen,0.0,1.0));outputColor=combined;%s%s}",
             cycle == 1 ? "compareAlpha" : "combined.a", pick_output,
             s->screen_effects ? "float z=recordDepth ? 1.0+round(gl_FragCoord.z*16777214.0) : 0.0;"
                                 "vec3 bytes=vec3(floor(z/65536.0),mod(floor(z/256.0),256.0),mod(z,256.0));"
                                 "float replace=(!blended || combined.a>=(recordDepth ? 0.95 : 0.05)) ? 1.0 : 0.0;"
                                 "sceneDepth=vec4(bytes/255.0,replace);"
                               : "");
    GLuint v = recomp_gles2_shader_parts(s, GL_VERTEX_SHADER, vs, sizeof(vs) / sizeof(*vs)),
           f = recomp_gles2_shader(s, GL_FRAGMENT_SHADER, fs);
    p->id = s->gl.CreateProgram();
    s->gl.AttachShader(p->id, v);
    s->gl.AttachShader(p->id, f);
    const char *attrs[] = {"position", "color", "uv", "fogv", "reciprocal"};
    for (unsigned i = 0; i < 5; i++)
        s->gl.BindAttribLocation(p->id, i, attrs[i]);
#ifdef RECOMP_GLES2_TEXTURE_PACKS
    if (s->editor)
        s->gl.BindAttribLocation(p->id, 5, "pickOffset");
#endif
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
    if (s->screen_effects) {
        p->record_depth = s->gl.GetUniformLocation(p->id, "recordDepth");
        p->blended = s->gl.GetUniformLocation(p->id, "blended");
    }
#ifdef RECOMP_GLES2_TEXTURE_PACKS
    if (s->editor)
        p->pick_base = s->gl.GetUniformLocation(p->id, "pickBase");
#endif
    p->prim = s->gl.GetUniformLocation(p->id, "prim");
    p->env = s->gl.GetUniformLocation(p->id, "env");
    p->fog = s->gl.GetUniformLocation(p->id, "fogc");
    p->lod = s->gl.GetUniformLocation(p->id, "lod");
    p->alpha = s->gl.GetUniformLocation(p->id, "alphatest");
    p->coverage_alpha = s->gl.GetUniformLocation(p->id, "coverageAlpha");
    p->fogen = s->gl.GetUniformLocation(p->id, "fogen");
    p->uv_bounds = s->gl.GetUniformLocation(p->id, "uvBounds");
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
