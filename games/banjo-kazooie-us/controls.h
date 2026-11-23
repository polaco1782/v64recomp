#pragma once
#include "renderer.h"

void banjo_controls_init(Runtime *rt, RecompRenderer *renderer);
bool banjo_controls_poll(Runtime *rt, RecompRenderer *renderer, RecompPad *pad);
