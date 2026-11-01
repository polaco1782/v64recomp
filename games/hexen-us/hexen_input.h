#pragma once
#include "runtime.h"
#include "mouse_input.h"

void hexen_input_init(void (*poll)(Runtime *, void *));
bool hexen_input_enabled(void);
bool hexen_input_playable(Runtime *rt);
void hexen_input_submit(RecompMouseFrame frame, RecompPad *pad);
