#ifndef BAD_APPLE_DEMO_H
#define BAD_APPLE_DEMO_H
#include "renderer.h"
#include "recomp_audio.h"
typedef void (*BadAppleDiagnostics)(Runtime *, RecompRenderer *, RecompAudio *, uint32_t frames);
int bad_apple_main(int argc, char **argv, BadAppleDiagnostics diagnostics, const char *wav);
#endif
