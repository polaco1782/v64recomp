#ifndef NIGHTMARE_INPUT_H
#define NIGHTMARE_INPUT_H
#include "runtime.h"
#include "mouse_input.h"
void nightmare_input_init(void);
bool nightmare_input_enabled(void);
bool nightmare_input_playable(Runtime *rt);
void nightmare_input_submit(RecompMouseFrame frame, RecompPad *pad);
#endif
