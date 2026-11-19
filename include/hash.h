#ifndef RECOMP_HASH_H
#define RECOMP_HASH_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
bool recomp_sha1(const void *data, size_t size, uint8_t digest[20]);
bool recomp_sha256(const void *data, size_t size, uint8_t digest[32]);
#ifdef __cplusplus
}
#endif
#endif
