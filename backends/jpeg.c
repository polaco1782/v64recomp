#include "recomp_jpeg.h"
#include "hle_internal.h"
#include "memory.h"
#include <openssl/evp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Keep JPEG callbacks separate from the audio adapter's HLE state. */
void recomp_jpeg_verbose(void *opaque, const char *message, ...) {
    (void)opaque;
    (void)message;
}
void recomp_jpeg_warn(void *opaque, const char *message, ...) {
    (void)opaque;
    va_list args;
    va_start(args, message);
    fputs("JPEG HLE: ", stderr);
    vfprintf(stderr, message, args);
    fputc('\n', stderr);
    va_end(args);
    recomp_fail("unsupported JPEG HLE operation", 0);
}
void recomp_jpeg_break(struct hle_t *hle, unsigned flags) {
    bool *done = hle->user_defined;
    if (flags != SP_STATUS_TASKDONE || *done)
        recomp_fail("invalid JPEG task completion", flags);
    *done = true;
}
static bool fingerprint(Runtime *rt, uint32_t address, uint32_t bytes, const char *hex) {
    if (!bytes || !hex || strlen(hex) != 64)
        return false;
    uint32_t physical = recomp_ram_address(rt, address, bytes);
    uint8_t *image = malloc(bytes);
    if (!image)
        return false;
    for (uint32_t i = 0; i < bytes; i++)
        image[i] = rt->rdram[(physical + i) ^ 3];
    uint8_t digest[32];
    unsigned length = 0;
    bool ok = EVP_Digest(image, bytes, digest, &length, EVP_sha256(), NULL) && length == 32;
    free(image);
    for (unsigned i = 0; ok && i < 32; i++) {
        unsigned expected;
        if (sscanf(hex + i * 2, "%2x", &expected) != 1 || expected != digest[i])
            ok = false;
    }
    return ok;
}
static bool physical_range(Runtime *rt, uint32_t address, uint64_t bytes, unsigned alignment) {
    return address % alignment == 0 && address < rt->rdram_size && bytes <= rt->rdram_size - address;
}
bool recomp_jpeg_decode_ps(Runtime *rt, const RecompTask *task, const RecompJpegProfile *profile) {
    if (!profile || rt->rdram_size > 0x1000000 || task->words[0] != 4 || task->words[1] || task->resumed ||
        task->words[12] % 4 || task->words[13] != 0x20)
        return false;
    for (unsigned i = 0; i < 6; i++)
        if (task->words[2 + i] != profile->descriptor[i])
            return false;
    for (unsigned i = 0; i < 3; i++) {
        if (profile->fingerprint_bytes[i] > task->words[3 + i * 2] ||
            !fingerprint(rt, task->words[2 + i * 2], profile->fingerprint_bytes[i], profile->sha256[i]))
            return false;
    }
    uint32_t header = recomp_ram_address(rt, task->words[12], task->words[13]);
    uint32_t buffer = (uint32_t)recomp_load(rt, task->words[12], 4);
    uint32_t count = (uint32_t)recomp_load(rt, task->words[12] + 4, 4);
    uint32_t mode = (uint32_t)recomp_load(rt, task->words[12] + 8, 4);
    if ((mode != 0 && mode != 2) || !count || !physical_range(rt, buffer, (uint64_t)count * (mode + 4) * 64 * 2, 2))
        return false;
    for (unsigned i = 0; i < 3; i++) {
        uint32_t table = (uint32_t)recomp_load(rt, task->words[12] + 12 + i * 4, 4);
        if (!physical_range(rt, table, 128, 2))
            return false;
    }
    /* The task header and every coefficient/table/output span are validated
     * before upstream's unchecked, 24-bit word-swapped memory accessors run. */
    _Alignas(16) uint8_t dmem[4096] = {0};
    bool done = false;
    struct hle_t hle = {.dram = rt->rdram, .dmem = dmem, .user_defined = &done};
    *dmem_u32(&hle, TASK_DATA_PTR) = header;
    jpeg_decode_PS(&hle);
    if (!done)
        recomp_fail("JPEG decoder did not complete", task->guest_address);
    return true;
}
