#pragma once
#include "renderer.h"

void turok_controls_init(void (*poll)(Runtime *, void *));
bool turok_controls_poll(Runtime *, RecompRenderer *, RecompPad *);
void turok_fov_adjust(int steps);
