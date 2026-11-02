#include "fov_control.h"
#include <math.h>
#include <stdio.h>

#define radians (3.14159265358979323846 / 180)

void recomp_fov_init(RecompFov *fov, double vertical, double aspect) {
    fov->original = 2 * atan(tan(vertical * radians / 2) * aspect) / radians;
    fov->horizontal = fov->original;
}
void recomp_fov_set(RecompFov *fov, double horizontal) {
    if (isfinite(horizontal))
        fov->horizontal = horizontal <= fov->original + 1e-9 ? fov->original : fmin(140, horizontal);
}
bool recomp_fov_adjust(RecompFov *fov, int steps) {
    double previous = fov->horizontal;
    recomp_fov_set(fov, previous + steps);
    if (previous == fov->horizontal)
        return false;
    fprintf(stderr, "FOV: %.1f degrees%s\n", fov->horizontal, recomp_fov_enabled(fov) ? "" : " (original)");
    return true;
}
bool recomp_fov_enabled(const RecompFov *fov) {
    return fov->horizontal > fov->original;
}
