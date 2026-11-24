#include "internal.h"
#include <stdio.h>

#ifdef RECOMP_GLES2_TEXTURE_PACKS
static uint64_t texture_editor_paused_ns;

uint64_t recomp_rdp_gles2_editor_paused_ns(void) {
    return texture_editor_paused_ns;
}

void recomp_rdp_gles2_enable_texture_editor(RecompRDPGles2 *s) {
    recomp_gles2_require_owner(s);
    if (!s->editor)
        s->editor = gles2_editor_create(s->texture_overrides);
    if (!s->editor)
        fprintf(stderr, "GLES2 texture editor unavailable: no texture override directory\n");
}

static bool run_texture_editor(RecompRDPGles2 *s) {
    const uint64_t start = recomp_monotonic_ns();
    recomp_ai_pause_output(s->rt, true);
    SDL_SetRelativeMouseMode(SDL_FALSE);
    s->mouse_captured = false;
    s->mouse_frame = (RecompMouseFrame){0};
    Gles2EditorResult result = gles2_editor_run(s->editor, s->window, s->max_texture_size);
    if (SDL_GL_GetCurrentContext() != s->context && SDL_GL_MakeCurrent(s->window, s->context) != 0)
        recomp_fail("cannot restore GLES2 context after texture editor", 0);
    if (result & GLES2_EDITOR_CHANGED) {
        s->original_textures = false;
        recomp_gles2_reload_textures(s);
    }
    if (!(result & GLES2_EDITOR_OPEN_FAILED))
        texture_editor_paused_ns += recomp_monotonic_ns() - start;
    recomp_ai_pause_output(s->rt, false);
    return !(result & GLES2_EDITOR_QUIT);
}
#endif

bool recomp_rdp_gles2_poll(RecompRDPGles2 *s) {
    recomp_gles2_require_owner(s);
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_QUIT)
            return false;
        if (event.type == SDL_DROPFILE || event.type == SDL_DROPTEXT) {
            SDL_free(event.drop.file);
            continue;
        }
        if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
            s->mouse_released = true;
            s->mouse_frame = (RecompMouseFrame){0};
        }
#ifdef RECOMP_GLES2_CHEATS
        if (s->cheats && gles2_cheats_event(s->cheats, &event))
            continue;
#endif
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
            if (key == SDL_SCANCODE_F8 && event.key.windowID == SDL_GetWindowID(s->window)) {
#ifdef RECOMP_GLES2_TEXTURE_PACKS
                if (s->editor) {
                    if (!run_texture_editor(s))
                        return false;
                } else {
                    fprintf(stderr, "GLES2 texture editor disabled or unavailable (try --texture-editor)\n");
                }
#else
                fprintf(stderr, "GLES2 texture editor unavailable in this build\n");
#endif
                continue;
            }
            if (key == SDL_SCANCODE_ESCAPE) {
                if (!s->input_config.mouse_enabled)
                    return false;
                s->mouse_released = s->mouse_captured;
            }
            if (key == SDL_SCANCODE_F6) {
                s->wireframe = !s->wireframe;
                fprintf(stderr, "GLES2 render mode: %s\n", s->wireframe ? "RDP mesh wireframe" : "textured");
            }
            if (key == SDL_SCANCODE_F4)
                s->mouse_released = false;
#ifdef RECOMP_GLES2_TEXTURE_PACKS
            if (key == SDL_SCANCODE_F5) {
                s->original_textures = !s->original_textures;
                recomp_gles2_reload_textures(s);
            }
#endif
            if (key == SDL_SCANCODE_F7) {
                if (!s->input_config.aspect_update) {
                    fprintf(stderr, "GLES2 widescreen is not available for this game or configuration.\n");
                } else {
                    s->expanded_aspect = !s->expanded_aspect;
                    if (s->expanded_aspect) {
                        int width, height;
                        recomp_gles2_drawable_size(s, &width, &height);
                        if (height <= 0 || (double)width <= (4.0 / 3.0) * height)
                            fprintf(stderr, "GLES2 widescreen requested: resize the window to a wider ratio.\n");
                        else
                            fprintf(stderr, "GLES2 widescreen requested for supported gameplay scenes.\n");
                    } else {
                        fprintf(stderr, "GLES2 widescreen off: original aspect.\n");
                    }
                }
            }
        }
        if (s->input_config.fov_adjust && (SDL_GetWindowFlags(s->window) & SDL_WINDOW_INPUT_FOCUS) &&
            (key == SDL_SCANCODE_KP_PLUS || key == SDL_SCANCODE_KP_MINUS))
            s->input_config.fov_adjust(key == SDL_SCANCODE_KP_PLUS ? 1 : -1);
    }
    return true;
}
bool recomp_rdp_gles2_input(RecompRDPGles2 *s, RecompPad *pad) {
    recomp_gles2_require_owner(s);
    s->mouse_frame = (RecompMouseFrame){0};
    s->camera_stick = (RecompCameraStick){0};
    if (!recomp_rdp_gles2_poll(s))
        return false;
    s->presentation_aspect = 0;
    if (s->input_config.aspect_update) {
        int width, height;
        recomp_gles2_drawable_size(s, &width, &height);
        float aspect = s->expanded_aspect && height > 0 ? fmaxf(4.0f / 3, (float)width / height) : 4.0f / 3;
        if (s->input_config.aspect_update(s->input_config.aspect_userdata, aspect))
            s->presentation_aspect = aspect;
    }
    bool focused = (SDL_GetWindowFlags(s->window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    bool menu_input = false;
#ifdef RECOMP_GLES2_CHEATS
    if (s->cheats)
        menu_input = gles2_cheats_update(s->cheats);
#endif
    bool capture =
        !menu_input && focused && s->input_config.mouse_enabled && s->input_config.mouse_gameplay && !s->mouse_released;
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
    if (!focused || menu_input)
        return true;
    const uint8_t *key = SDL_GetKeyboardState(NULL);
    bool keyboard_c = s->input_config.keyboard_c_buttons;
    recomp_input_keyboard_stick(pad, key, keyboard_c, s->input_config.wasd_stick);
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
            if (s->input_config.separate_mouse_actions)
                pad->buttons &= 0x1000;
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
    if (s->input_config.separate_mouse_actions) {
        if (key[SDL_SCANCODE_TAB])
            pad->buttons |= 0x1000;
        if (key[SDL_SCANCODE_E])
            pad->buttons |= 0x4000;
    }
    if (s->input_config.wasd_stick && key[SDL_SCANCODE_TAB])
        pad->buttons |= 0x1000;
    for (unsigned i = 0; i < sizeof(map) / sizeof(*map); i++) {
        if (s->input_config.wasd_stick && (map[i].key == SDL_SCANCODE_A || map[i].key == SDL_SCANCODE_S))
            continue;
        if (key[map[i].key])
            pad->buttons |= map[i].button;
    }
    if (!s->input_config.wasd_stick && pad->stick_x && pad->stick_y) {
        pad->stick_x = (int8_t)(pad->stick_x * 7 / 10);
        pad->stick_y = (int8_t)(pad->stick_y * 7 / 10);
    }
    recomp_input_poll(s->input, pad, s->input_config.gamepad_camera ? &s->camera_stick : NULL);
    return true;
}
void recomp_rdp_gles2_set_input_config(RecompRDPGles2 *s, const RecompGles2InputConfig *config) {
    recomp_gles2_require_owner(s);
    s->input_config = *config;
}
RecompMouseFrame recomp_rdp_gles2_mouse_frame(RecompRDPGles2 *s) {
    recomp_gles2_require_owner(s);
    RecompMouseFrame result = s->mouse_frame;
    s->mouse_frame.dx = s->mouse_frame.dy = 0;
    s->mouse_frame.wheel = 0;
    return result;
}
RecompCameraStick recomp_rdp_gles2_camera_stick(RecompRDPGles2 *s) {
    recomp_gles2_require_owner(s);
    return s->camera_stick;
}
