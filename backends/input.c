#include "input.h"
#include <SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

struct RecompInput {
    SDL_GameController *controller;
    uint16_t held_axes;
};

static void connect_controller(RecompInput *input) {
    if (input->controller && !SDL_GameControllerGetAttached(input->controller)) {
        SDL_GameControllerClose(input->controller);
        input->controller = NULL;
        input->held_axes = 0;
        fprintf(stderr, "Gamepad disconnected; keyboard input remains available.\n");
    }
    if (input->controller)
        return;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i))
            continue;
        input->controller = SDL_GameControllerOpen(i);
        if (!input->controller)
            continue;
        const char *name = SDL_GameControllerName(input->controller);
        fprintf(stderr, "Gamepad: %s (analog left stick, N64 controls)\n", name ? name : "unknown controller");
        break;
    }
}
RecompInput *recomp_input_create(void) {
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "Gamepad initialization failed: %s\n", SDL_GetError());
        return NULL;
    }
    RecompInput *input = calloc(1, sizeof(*input));
    if (!input)
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
    return input;
}
void recomp_input_destroy(RecompInput *input) {
    if (!input)
        return;
    if (input->controller)
        SDL_GameControllerClose(input->controller);
    free(input);
    SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
}
static float axis(RecompInput *input, SDL_GameControllerAxis which) {
    Sint16 value = SDL_GameControllerGetAxis(input->controller, which);
    return value / (value < 0 ? 32768.0f : 32767.0f);
}
static void axis_button(RecompInput *input, float value, uint16_t bit) {
    /* Hysteresis prevents C-buttons or triggers chattering at the boundary. */
    if (value >= 0.5f)
        input->held_axes |= bit;
    else if (value <= 0.4f)
        input->held_axes &= (uint16_t)~bit;
}
void recomp_input_poll(RecompInput *input, RecompPad *pad) {
    if (!input)
        return;
    SDL_GameControllerUpdate();
    connect_controller(input);
    if (!input->controller)
        return;
    pad->connected = true;
    const struct {
        SDL_GameControllerButton source;
        uint16_t target;
    } bindings[] = {{SDL_CONTROLLER_BUTTON_A, 0x8000},
                    {SDL_CONTROLLER_BUTTON_X, 0x4000},
                    {SDL_CONTROLLER_BUTTON_B, 0x0004},
                    {SDL_CONTROLLER_BUTTON_Y, 0x0002},
                    {SDL_CONTROLLER_BUTTON_START, 0x1000},
                    {SDL_CONTROLLER_BUTTON_DPAD_UP, 0x0800},
                    {SDL_CONTROLLER_BUTTON_DPAD_DOWN, 0x0400},
                    {SDL_CONTROLLER_BUTTON_DPAD_LEFT, 0x0200},
                    {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, 0x0100},
                    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, 0x0020},
                    {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, 0x0010},
                    {SDL_CONTROLLER_BUTTON_RIGHTSTICK, 0x0008}};
    for (unsigned i = 0; i < sizeof(bindings) / sizeof(*bindings); i++)
        if (SDL_GameControllerGetButton(input->controller, bindings[i].source))
            pad->buttons |= bindings[i].target;
    axis_button(input, axis(input, SDL_CONTROLLER_AXIS_TRIGGERLEFT), 0x2000);
    axis_button(input, axis(input, SDL_CONTROLLER_AXIS_TRIGGERRIGHT), 0x0010);
    float right_x = axis(input, SDL_CONTROLLER_AXIS_RIGHTX), right_y = axis(input, SDL_CONTROLLER_AXIS_RIGHTY);
    axis_button(input, -right_y, 0x0008);
    axis_button(input, right_y, 0x0004);
    axis_button(input, -right_x, 0x0002);
    axis_button(input, right_x, 0x0001);
    pad->buttons |= input->held_axes;

    /* A radial 12% deadzone preserves direction and rescales the remaining
     * travel continuously. Use a nominal 80-unit radius like keyboard input;
     * diagonal magnitude stays within rounding error of the same range. */
    const float deadzone = 0.12f;
    float x = axis(input, SDL_CONTROLLER_AXIS_LEFTX), y = -axis(input, SDL_CONTROLLER_AXIS_LEFTY);
    float radius = sqrtf(x * x + y * y);
    if (radius <= deadzone)
        return;
    float travel = (fminf(radius, 1.0f) - deadzone) / (1.0f - deadzone);
    int sx = (int)lroundf(80.0f * travel * x / radius), sy = (int)lroundf(80.0f * travel * y / radius);
    if (sx * sx + sy * sy > pad->stick_x * pad->stick_x + pad->stick_y * pad->stick_y) {
        pad->stick_x = (int8_t)sx;
        pad->stick_y = (int8_t)sy;
    }
}
