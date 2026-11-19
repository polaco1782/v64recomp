#include "hash.h"
#ifndef __EMSCRIPTEN__
#include <openssl/evp.h>
bool recomp_sha1(const void *data, size_t size, uint8_t digest[20]) {
    unsigned length;
    return EVP_Digest(data, size, digest, &length, EVP_sha1(), NULL) && length == 20;
}
bool recomp_sha256(const void *data, size_t size, uint8_t digest[32]) {
    unsigned length;
    return EVP_Digest(data, size, digest, &length, EVP_sha256(), NULL) && length == 32;
}
#else
/* FIPS 180-4 SHA-1/SHA-256 for synchronous ROM/microcode fingerprints.
 * No host crypto library or asynchronous browser callback is needed. */
#include <string.h>
static uint32_t rotate(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}
static uint32_t word(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void block(uint32_t h[8], const uint8_t *p, bool sha256) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t w[80];
    unsigned rounds = sha256 ? 64 : 80;
    for (unsigned i = 0; i < 16; i++)
        w[i] = word(p + 4 * i);
    for (unsigned i = 16; i < rounds; i++) {
        if (sha256) {
            uint32_t x = w[i - 15], y = w[i - 2];
            w[i] = w[i - 16] + (rotate(x, 7) ^ rotate(x, 18) ^ (x >> 3)) + w[i - 7] +
                   (rotate(y, 17) ^ rotate(y, 19) ^ (y >> 10));
        } else
            w[i] = rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 31);
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], v = h[7];
    for (unsigned i = 0; i < rounds; i++) {
        if (sha256) {
            uint32_t t1 = v + (rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            uint32_t t2 = (rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            v = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        } else {
            uint32_t mix = i < 20   ? ((b & c) | (~b & d))
                           : i < 40 ? (b ^ c ^ d)
                           : i < 60 ? ((b & c) | (b & d) | (c & d))
                                    : (b ^ c ^ d);
            uint32_t constant = i < 20 ? 0x5a827999 : i < 40 ? 0x6ed9eba1 : i < 60 ? 0x8f1bbcdc : 0xca62c1d6;
            uint32_t t = rotate(a, 27) + mix + e + constant + w[i];
            e = d;
            d = c;
            c = rotate(b, 2);
            b = a;
            a = t;
        }
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    if (sha256) {
        h[5] += f;
        h[6] += g;
        h[7] += v;
    }
}
static bool digest(const void *data, size_t size, uint8_t *out, bool sha256) {
    if ((!data && size) || !out || (uint64_t)size > UINT64_MAX / 8)
        return false;
    uint32_t h[8];
    const uint32_t init256[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const uint32_t init1[8] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0, 0, 0, 0};
    memcpy(h, sha256 ? init256 : init1, sizeof(h));
    const uint8_t *p = data;
    uint64_t bits = (uint64_t)size * 8;
    while (size >= 64) {
        block(h, p, sha256);
        p += 64;
        size -= 64;
    }
    uint8_t tail[128] = {0};
    if (size)
        memcpy(tail, p, size);
    tail[size] = 0x80;
    unsigned padded = size < 56 ? 64 : 128;
    for (unsigned i = 0; i < 8; i++)
        tail[padded - 1 - i] = (uint8_t)(bits >> (8 * i));
    block(h, tail, sha256);
    if (padded == 128)
        block(h, tail + 64, sha256);
    for (unsigned i = 0; i < (sha256 ? 32u : 20u); i++)
        out[i] = (uint8_t)(h[i / 4] >> (24 - 8 * (i % 4)));
    return true;
}
bool recomp_sha1(const void *data, size_t size, uint8_t out[20]) {
    return digest(data, size, out, false);
}
bool recomp_sha256(const void *data, size_t size, uint8_t out[32]) {
    return digest(data, size, out, true);
}
#endif
