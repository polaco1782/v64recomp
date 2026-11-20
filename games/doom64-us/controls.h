#pragma once
#include "renderer.h"

void doom64_controls_init(RecompRenderer *renderer);
bool doom64_controls_poll(Runtime *rt, RecompRenderer *renderer, RecompPad *pad);
