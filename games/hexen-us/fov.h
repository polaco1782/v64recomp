#pragma once
#include "runtime.h"

/* Start at stock; adjustments clamp horizontal degrees to stock..140. */
void hexen_fov_init(void);
void hexen_fov_adjust(int steps);
