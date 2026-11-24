#ifndef RECOMP_JPEG_H
#define RECOMP_JPEG_H
#include "runtime.h"
#include "rsp.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct RecompJpegProfile {
    uint32_t descriptor[6];
    const uint8_t *data;
    size_t data_size;
    const RecompRspProgram *program;
} RecompJpegProfile;
extern const RecompJpegProfile game_jpeg_profile;
bool recomp_jpeg_submit(Runtime *, const RecompTask *);
#ifdef __cplusplus
}
#endif
#endif
