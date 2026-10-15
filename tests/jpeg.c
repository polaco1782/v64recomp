#include "recomp_jpeg.h"
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void check(bool ok, const char *message) {
    if (!ok) { fprintf(stderr, "JPEG fixture: %s\n", message); exit(1); }
}
static void rejected(Runtime *rt, const RecompTask *task, const RecompJpegProfile *profile) {
    uint8_t *before = malloc(rt->rdram_size);
    check(before != NULL, "snapshot allocation");
    memcpy(before, rt->rdram, rt->rdram_size);
    check(!recomp_jpeg_decode_ps(rt, task, profile), "malformed task accepted");
    check(!memcmp(before, rt->rdram, rt->rdram_size), "rejected task modified RAM");
    free(before);
}
int main(void) {
    Runtime rt;
    check(recomp_runtime_init(&rt, 65536, NULL, 0, NULL, 1), "runtime init");
    /* Synthetic identity images exercise recognition without embedding ROM data. */
    char hash[65]; uint8_t digest[32], image[32] = {0}; unsigned length;
    check(EVP_Digest(image, sizeof(image), digest, &length, EVP_sha256(), NULL) && length == 32, "SHA-256");
    for (unsigned i = 0; i < 32; i++) snprintf(hash + i * 2, 3, "%02x", digest[i]);
    RecompJpegProfile profile = {.descriptor = {0x100, 32, 0x200, 32, 0x300, 32},
        .fingerprint_bytes = {32, 32, 32}, .sha256 = {hash, hash, hash}};
    RecompTask task = {.words = {4, 0, 0x100, 32, 0x200, 32, 0x300, 32,
                               0, 0, 0, 0, 0x1000, 0x20, 0, 0}};
    for (unsigned t = 0; t < 3; t++) {
        recomp_store(&rt, 0x100c + t * 4, 0x1800 + t * 128, 4);
        for (unsigned i = 0; i < 64; i++) recomp_store(&rt, 0x1800 + t * 128 + i * 2, 16, 2);
    }
    recomp_store(&rt, 0x1000, 0x2000, 4); recomp_store(&rt, 0x1004, 2, 4);
    for (unsigned mode = 0; mode <= 2; mode += 2) {
        unsigned stride = (mode + 4) * 128, height = mode ? 16 : 8;
        memset(rt.rdram + 0x2000, 0, stride * 2);
        recomp_store(&rt, 0x1008, mode, 4);
        const int dc[4] = {-64, 0, 64, 32};
        const uint16_t rgba[4] = {0x0001, 0x8421, 0xffff, 0xc631};
        for (unsigned i = 0; i < mode + 2; i++) recomp_store(&rt, 0x2000 + i * 128, (uint16_t)dc[i], 2);
        recomp_store(&rt, 0x1ffc, 0xdeadbeef, 4);
        recomp_store(&rt, 0x2000 + stride * 2, 0xdeadbeef, 4);
        check(recomp_jpeg_decode_ps(&rt, &task, &profile), "valid task rejected");
        for (unsigned y = 0; y < height; y++) for (unsigned x = 0; x < 16; x++) {
            unsigned q = (y / 8) * 2 + x / 8;
            check(recomp_load(&rt, 0x2000 + (y * 16 + x) * 2, 2) == rgba[q], "tile layout or DC color");
            check(recomp_load(&rt, 0x2000 + stride + (y * 16 + x) * 2, 2) == 0x8421, "macroblock stride");
        }
        check(recomp_load(&rt, 0x1ffc, 4) == 0xdeadbeef
            && recomp_load(&rt, 0x2000 + stride * 2, 4) == 0xdeadbeef, "output exceeded buffer");
    }
    /* Distinguish Cb/Cr and verify saturated blue/red RGBA5551 conversion. */
    memset(rt.rdram + 0x2000, 0, 0x600);
    recomp_store(&rt, 0x2200, 64, 2); /* First macroblock: Cb DC. */
    recomp_store(&rt, 0x2580, 64, 2); /* Second macroblock: Cr DC. */
    check(recomp_jpeg_decode_ps(&rt, &task, &profile), "chroma task rejected");
    for (unsigned i = 0; i < 256; i++) {
        check(recomp_load(&rt, 0x2000 + i * 2, 2) == 0x82bf, "Cb color conversion");
        check(recomp_load(&rt, 0x2300 + i * 2, 2) == 0xf921, "Cr color conversion");
    }
    RecompTask bad = task; bad.resumed = true; rejected(&rt, &bad, &profile);
    bad = task; bad.words[0] = 1; rejected(&rt, &bad, &profile);
    bad = task; bad.words[1] = 1; rejected(&rt, &bad, &profile);
    bad = task; bad.words[4] += 8; rejected(&rt, &bad, &profile);
    bad = task; bad.words[12]++; rejected(&rt, &bad, &profile);
    bad = task; bad.words[13] = 24; rejected(&rt, &bad, &profile);
    recomp_store(&rt, 0x200, 1, 1); rejected(&rt, &task, &profile); recomp_store(&rt, 0x200, 0, 1);
    recomp_store(&rt, 0x1008, 1, 4); rejected(&rt, &task, &profile); recomp_store(&rt, 0x1008, 2, 4);
    recomp_store(&rt, 0x1004, UINT32_MAX, 4); rejected(&rt, &task, &profile); recomp_store(&rt, 0x1004, 2, 4);
    recomp_store(&rt, 0x1000, 0xff00, 4); rejected(&rt, &task, &profile); recomp_store(&rt, 0x1000, 0x2000, 4);
    recomp_store(&rt, 0x100c, 0x1801, 4); rejected(&rt, &task, &profile);
    recomp_store(&rt, 0x100c, 0xffc0, 4); rejected(&rt, &task, &profile);
    recomp_store(&rt, 0x100c, 0x1800, 4);
    task.words[12] = 0x80001000; /* Normal game's cached RAM alias. */
    /* Exercise AC coefficients and large intermediate values under sanitizers. */
    uint32_t random = 1;
    for (unsigned trial = 0; trial < 32; trial++) {
        for (unsigned i = 0; i < 6 * 64 * 2; i++) {
            random = random * 1664525u + 1013904223u;
            recomp_store(&rt, 0x2000 + i * 2, (uint16_t)((int)(random >> 20) - 2048), 2);
        }
        check(recomp_jpeg_decode_ps(&rt, &task, &profile), "AC coefficient task rejected");
        check(recomp_load(&rt, 0x1ffc, 4) == 0xdeadbeef
            && recomp_load(&rt, 0x2600, 4) == 0xdeadbeef, "AC output exceeded buffer");
    }
    recomp_runtime_destroy(&rt);
    puts("PASS: JPEG 4:2:2/4:2:0 colors, tile layout, macroblock stride, bounds and microcode rejection");
}
