#ifndef QUAKE2_INPUT_H
#define QUAKE2_INPUT_H
#include "runtime.h"
#include "mouse_input.h"

void quake2_input_init(void);
bool quake2_input_enabled(void);
bool quake2_input_playable(Runtime *rt);
void quake2_input_submit(RecompMouseFrame frame);
#endif
