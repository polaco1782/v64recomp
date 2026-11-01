#pragma once
#include "runtime.h"

/* Horizontal degrees at the original gameplay aspect ratio. NULL, empty and
 * zero retain the original guest projection and visibility setup. */
bool hexen_fov_configure(const char *setting);
