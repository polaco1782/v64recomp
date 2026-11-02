#pragma once
#include "renderer.h"

void quake_controls_init(void (*poll)(Runtime *, void *));
bool quake_controls_poll(Runtime *, RecompRenderer *, RecompPad *);
void quake_fov_adjust(int steps);
