#pragma once
#include "runtime.h"

/* Start at stock; adjustments clamp horizontal degrees to stock..140. */
void duke_fov_init(void);
void duke_fov_adjust(int steps);
