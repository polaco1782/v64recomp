#pragma once
#include "runtime.h"

/* Start at stock; adjustments clamp horizontal degrees to stock..140.
 * Native zoom continues to control optical magnification. */
void goldeneye_fov_init(void);
void goldeneye_fov_adjust(int steps);
bool goldeneye_aspect_update(void *runtime, float aspect);
