#ifndef SM64_CONTROLS_H
#define SM64_CONTROLS_H
#include "renderer.h"

void sm64_controls_init(RecompRenderer *);
bool sm64_controls_poll(Runtime *, RecompRenderer *, RecompPad *);

#endif
