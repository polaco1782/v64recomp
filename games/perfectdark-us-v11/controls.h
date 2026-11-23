#ifndef PERFECTDARK_CONTROLS_H
#define PERFECTDARK_CONTROLS_H
#include "renderer.h"

void perfectdark_controls_init(RecompRenderer *renderer);
bool perfectdark_controls_poll(Runtime *rt, RecompRenderer *renderer, RecompPad *pad);

#endif
