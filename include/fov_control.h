#pragma once
#include <stdbool.h>

/* Horizontal degrees at the game's reference aspect; minimum is exactly stock. */
typedef struct RecompFov {
    double original, horizontal;
} RecompFov;
void recomp_fov_init(RecompFov *, double vertical, double aspect);
void recomp_fov_set(RecompFov *, double horizontal);
bool recomp_fov_adjust(RecompFov *, int steps);
bool recomp_fov_enabled(const RecompFov *);
