#pragma once
#include "runtime.h"

/* Horizontal degrees referenced to 4:3. Unset, empty or zero keeps the guest's
 * original FOV. Native zoom continues to control optical magnification. */
bool goldeneye_fov_configure(const char *setting);
