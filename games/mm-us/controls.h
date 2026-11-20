#pragma once
#include "renderer.h"

void mm_controls_init(RecompRenderer *renderer);
bool mm_controls_poll(Runtime *rt, RecompRenderer *renderer, RecompPad *pad);
