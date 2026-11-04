#pragma once
#include "renderer.h"

void rayman2_controls_init(void (*poll)(Runtime *, void *));
bool rayman2_controls_poll(Runtime *, RecompRenderer *, RecompPad *);
void rayman2_fov_adjust(int steps);
