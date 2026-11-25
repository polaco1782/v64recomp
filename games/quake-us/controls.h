#pragma once
#include "renderer.h"

void quake_controls_init(void (*poll)(Runtime *, void *));
bool quake_controls_poll(Runtime *, RecompRenderer *, RecompPad *);
bool quake_single_player_view(Runtime *);
void quake_fov_adjust(int steps);

#include "torch_lights.h"
void quake_torch_lights(Runtime *, RecompTorchLights *);
