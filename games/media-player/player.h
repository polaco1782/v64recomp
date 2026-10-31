#ifndef MEDIA_PLAYER_H
#define MEDIA_PLAYER_H
#include <stdint.h>
typedef void (*MediaCapture)(const int16_t *, int samples, int rate);
int media_player_main(int argc, char **argv, MediaCapture capture);
#endif
