#pragma once
#include "runtime.h"
#include "renderer.h"

void oot_controls_init(void (*poll)(Runtime *, void *));
void oot_fov_init(void);
void oot_fov_adjust(int steps);
bool oot_controls_poll(Runtime *, RecompRenderer *, RecompPad *);

#ifdef RECOMP_WITH_GLES2
#include "torch_lights.h"
void oot_torch_lights(Runtime *, RecompTorchLights *);
#endif
