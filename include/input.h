#ifndef RECOMP_INPUT_H
#define RECOMP_INPUT_H
#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct RecompInput RecompInput;
typedef struct RecompCameraStick {
    float x, y; /* Normalized after the radial deadzone; right/down are positive. */
    bool active;
} RecompCameraStick;
/* SDL gamepad input, owned and polled by the SDL window's host thread. Keyboard
 * input remains available. poll() merges into a freshly initialized pad, keeping
 * the stronger of its existing stick vector and the controller's analog vector.
 * The first recognized controller drives port 1; unplugging permits replacement.
 * With a camera output, the right stick supplies held camera axes and the D-pad
 * supplies C-buttons. NULL keeps the original N64 mapping. The output is reset
 * each poll; active means a controller is attached, even with a centered stick. */
RecompInput *recomp_input_create(void);
void recomp_input_destroy(RecompInput *);
void recomp_input_poll(RecompInput *, RecompPad *, RecompCameraStick *camera);
#ifdef __cplusplus
}
#endif
#endif
