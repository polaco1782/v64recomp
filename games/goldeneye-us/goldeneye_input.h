#ifndef GOLDENEYE_INPUT_H
#define GOLDENEYE_INPUT_H
#include "runtime.h"
#include "mouse_input.h"
void goldeneye_input_init(Runtime *);
void goldeneye_input_destroy(void);
bool goldeneye_input_enabled(void);
bool goldeneye_input_playable(Runtime *);
void goldeneye_input_submit(RecompMouseFrame);
#endif
