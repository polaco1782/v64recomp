#include "internal.h"

#include <stddef.h>

static void clear_wireframe_background(RecompRDPGles2 *s) {
    s->gl.Disable(GL_SCISSOR_TEST);
    s->gl.ClearColor(0, 0, 0, 1);
    s->gl.Clear(GL_COLOR_BUFFER_BIT);
}

void recomp_gles2_toggle_wireframe(RecompRDPGles2 *s) {
    recomp_gles2_flush(s);
    s->wireframe = !s->wireframe;
    /* VI may still present an older color image before the next RDP draw. */
    for (unsigned i = 0; i < 8; i++) {
        Surface *f = &s->surfaces[i];
        if (f->valid) {
            s->gl.BindFramebuffer(GL_FRAMEBUFFER, f->framebuffer);
            clear_wireframe_background(s);
            f->has_midpoint = false;
            f->clear_before_wireframe = false;
        }
    }
    s->last_display = NULL;
    s->gl.BindFramebuffer(GL_FRAMEBUFFER, s->surface->framebuffer);
}

void recomp_gles2_begin_wireframe_draw(RecompRDPGles2 *s) {
    /* Games can cover the previous frame with opaque geometry instead of a
     * color clear. Lines alone would leave trails on rotating color images. */
    if (s->surface->clear_before_wireframe && s->color_siz >= 2) {
        clear_wireframe_background(s);
        s->surface->clear_before_wireframe = false;
    }
}

/* ES has no polygon mode. Give each RDP mesh triangle three explicit
 * edges, using the same position, viewport, scissor and depth state. */
static void wireframe_initialize(RecompRDPGles2 *s) {
    const char *vs = "#version 300 es\nin vec4 position;void main(){gl_Position=position;}";
    const char *fs = "#version 300 es\nprecision mediump float;uniform vec4 wireColor;layout(location=0) out vec4 "
                     "outputColor;layout(location=1) out vec4 sceneDepth;void "
                     "main(){outputColor=wireColor;sceneDepth=vec4(0,0,0,1);}";
    GLuint v = recomp_gles2_shader(s, GL_VERTEX_SHADER, vs);
    GLuint f = recomp_gles2_shader(s, GL_FRAGMENT_SHADER, fs);
    s->wireframe_program = s->gl.CreateProgram();
    s->gl.AttachShader(s->wireframe_program, v);
    s->gl.AttachShader(s->wireframe_program, f);
    s->gl.BindAttribLocation(s->wireframe_program, 0, "position");
    s->gl.LinkProgram(s->wireframe_program);
    s->gl.DeleteShader(v);
    s->gl.DeleteShader(f);
    GLint ok;
    s->gl.GetProgramiv(s->wireframe_program, GL_LINK_STATUS, &ok);
    if (!ok)
        recomp_fail("GLES2 wireframe program link failed", 0);
    s->wireframe_color = s->gl.GetUniformLocation(s->wireframe_program, "wireColor");

    GLushort indices[RECOMP_GLES2_BATCH_VERTICES * 2];
    for (unsigned i = 0; i < RECOMP_GLES2_BATCH_VERTICES; i += 3) {
        const GLushort a = (GLushort)i, b = (GLushort)(i + 1), c = (GLushort)(i + 2);
        indices[i * 2] = a;
        indices[i * 2 + 1] = b;
        indices[i * 2 + 2] = b;
        indices[i * 2 + 3] = c;
        indices[i * 2 + 4] = c;
        indices[i * 2 + 5] = a;
    }
    s->gl.GenBuffers(1, &s->wireframe_indices);
    s->gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, s->wireframe_indices);
    s->gl.BufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);
}

void recomp_gles2_draw_wireframe(RecompRDPGles2 *s, const Program *program) {
    /* Use the UltraHLE palette and the first sampled tile. The color
     * follows the original RDRAM image, not a GL name or replacement hash. */
    static const uint32_t colors[] = {
        0x000099ff, 0x009900ff, 0x009999ff, 0x990000ff, 0x990099ff, 0x999900ff, 0x808080ff, 0x333333ff,
        0x0000ffff, 0x00ff00ff, 0x00ffffff, 0xff0000ff, 0xff00ffff, 0xffff00ff, 0xffffffff,
    };
    uint32_t color = 0xffffffff;
    if (s->texture_enabled && (program->texture[0] || program->texture[1])) {
        unsigned offset = !program->texture[0] && program->cycle != 0;
        const Tile *tile = &s->tiles[(s->texture_tile + offset) & 7];
        uint32_t source = s->tmem_source[tile->tmem];
        if (source)
            color = colors[(source - 1) % (sizeof(colors) / sizeof(colors[0]))];
    }
    float rgba[4] = {(float)(color >> 24) / 255, (float)((color >> 16) & 255) / 255, (float)((color >> 8) & 255) / 255,
                     1};
    if (!s->wireframe_program)
        wireframe_initialize(s);
    s->gl.UseProgram(s->wireframe_program);
    s->gl.Uniform4fv(s->wireframe_color, 1, rgba);
    s->gl.Disable(GL_BLEND);
    if (s->drawing_rectangle)
        s->gl.DrawArrays(GL_LINE_LOOP, 0, 4);
    else {
        s->gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, s->wireframe_indices);
        s->gl.DrawElements(GL_LINES, (GLsizei)(s->batch_count * 2), GL_UNSIGNED_SHORT, NULL);
    }
    s->gl.BindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
}

void recomp_gles2_destroy_wireframe(RecompRDPGles2 *s) {
    if (s->wireframe_indices)
        s->gl.DeleteBuffers(1, &s->wireframe_indices);
    if (s->wireframe_program)
        s->gl.DeleteProgram(s->wireframe_program);
}
