#ifndef QUAKE2_INPUT_H
#define QUAKE2_INPUT_H
#include "runtime.h"
#include "mouse_input.h"

/* Refresh live input on the owner thread immediately before consuming a
 * guest command. Replay mode disables mouse injection and this callback. */
void quake2_input_init(void (*poll)(Runtime *, void *));
bool quake2_input_enabled(void);
bool quake2_input_playable(Runtime *rt);
void quake2_input_submit(RecompMouseFrame frame);
#endif
