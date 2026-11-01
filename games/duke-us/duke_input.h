#pragma once
#include "runtime.h"
#include "renderer.h"

void duke_input_init(void (*poll)(Runtime *, void *));
bool duke_input_playable(Runtime *);
void duke_input_submit(RecompMouseFrame);
bool duke_input_poll(Runtime *, RecompRenderer *, RecompPad *);
