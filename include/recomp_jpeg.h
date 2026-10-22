#ifndef RECOMP_JPEG_H
#define RECOMP_JPEG_H
#include "runtime.h"
#ifdef __cplusplus
extern "C" {
#endif

typedef struct RecompJpegProfile {
    uint32_t descriptor[6];        /* Boot/code/data addresses and task sizes. */
    uint32_t fingerprint_bytes[3]; /* Initialized images, excluding DMEM BSS. */
    const char *sha256[3];
} RecompJpegProfile;

/* PS/OOT JPEG microcode: dequantize/IDCT CPU-decoded coefficients and write
 * RGBA5551 tiles in place. Runs on the owner, before the caller posts SP_DONE.
 * Rejects unknown microcode, resumed tasks, and invalid operands without writes. */
bool recomp_jpeg_decode_ps(Runtime *, const RecompTask *, const RecompJpegProfile *);
#ifdef __cplusplus
}
#endif
#endif
