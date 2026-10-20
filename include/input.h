#ifndef RECOMP_INPUT_H
#define RECOMP_INPUT_H
#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct RecompInput RecompInput;
/* SDL gamepad input, owned and polled by the SDL window's host thread. Keyboard
 * input remains available. poll() merges into a freshly initialized pad, keeping
 * the stronger of its existing stick vector and the controller's analog vector.
 * The first recognized controller drives port 1; unplugging permits replacement. */
RecompInput *recomp_input_create(void);
void recomp_input_destroy(RecompInput *);
void recomp_input_poll(RecompInput *, RecompPad *);
#ifdef __cplusplus
}
#endif
#endif
