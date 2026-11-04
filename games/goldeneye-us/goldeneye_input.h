#ifndef GOLDENEYE_INPUT_H
#define GOLDENEYE_INPUT_H
#include "runtime.h"
#include "mouse_input.h"
#include "input.h"
void goldeneye_input_init(Runtime *);
void goldeneye_input_destroy(void);
bool goldeneye_input_enabled(void);
bool goldeneye_input_playable(Runtime *);
bool goldeneye_input_menu(Runtime *);
bool goldeneye_input_gamepad_mode(Runtime *);
/* Submit motion and merge menu mouse buttons into the host controller sample. */
void goldeneye_input_submit(Runtime *, RecompMouseFrame, RecompCameraStick, RecompPad *);
#endif
