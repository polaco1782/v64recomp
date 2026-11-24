#pragma once
#include "renderer.h"

void acclaim_controls_init(RecompRenderer *renderer);
bool acclaim_controls_poll(Runtime *rt, RecompRenderer *renderer, RecompPad *pad);
