#pragma once
#include "runtime.h"
#include "renderer.h"

void oot_controls_init(void (*poll)(Runtime *, void *));
bool oot_fov_configure(const char *setting);
bool oot_controls_playable(Runtime *);
bool oot_controls_poll(Runtime *, RecompRenderer *, RecompPad *);
/* Shared by live input and the focused Debug check. */
void oot_controls_submit(RecompMouseFrame, RecompPad *);
