/* ROM-free fixed-point video fixtures.
 * clang -std=c11 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
 *   -Igames/re2-us-v1 games/re2-us-v1/debug/check_video_dsp.c \
 *   games/re2-us-v1/video_dsp.c -o build/re2-us-v1-debug/check-video-dsp
 * ASAN_OPTIONS=detect_leaks=0 build/re2-us-v1-debug/check-video-dsp */
#include "video_dsp.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static void word(uint8_t *d, unsigned off, uint32_t value) {
    for (unsigned i = 0; i < 4; i++)
        d[off + i] = (uint8_t)(value >> (24 - i * 8));
}
static uint16_t pixel(const uint8_t *dm) {
    return (uint16_t)(dm[0x780] << 8 | dm[0x781]);
}
int main(void) {
    uint8_t data[128] = {0}, dm[4096] = {0};
    word(data, 8, 4);
    word(data, 12, 2);
    word(data, 16, 0x8000);
    word(data, 20, 0x8000);
    word(data, 24, 8);
    word(data, 32, 0x8000);
    word(data, 36, 0x8000);
    const uint32_t constants[] = {0xffff7fff, 0x00030004, 0x000100f8, 0x40000008, 0x00800007,
                                  0x800000ff, 0x00040000, 0,          0x03c00000};
    for (unsigned i = 0; i < sizeof(constants) / sizeof(constants[0]); i++)
        word(data, 0x40 + i * 4, constants[i]);
    struct re2_rsp_resize r;
    struct re2_rsp_dither state;
    assert(re2_rsp_resize_read(data, &r));
    assert(r.x0 == 0x8000 && r.y0 == 0x8000);
    re2_rsp_resize_begin(&state, 0, 0);
    memset(dm, 128, 1920);
    re2_rsp_resize_row(dm, &r, &state, 0);
    assert(pixel(dm) == 0x8421);
    memset(dm, 255, 1920);
    re2_rsp_resize_row(dm, &r, &state, 1);
    assert(pixel(dm) == 0xffff);
    memset(dm, 0, sizeof(dm));
    for (unsigned k = 0; k < 3; k++) {
        dm[3 + k] = 64;
        dm[960 + k] = 128;
        dm[963 + k] = 255;
    }
    re2_rsp_resize_begin(&state, 0, 0);
    re2_rsp_resize_row(dm, &r, &state, 0);
    assert(pixel(dm) == 0x739d);
    /* The following row has integral Y; fractional Y must advance with n. */
    re2_rsp_resize_row(dm, &r, &state, 1);
    assert(pixel(dm) != 0x739d);
    word(data, 8, 1028);
    assert(!re2_rsp_resize_read(data, &r));
    word(data, 8, 4);
    word(data, 0x58, 0);
    assert(!re2_rsp_resize_read(data, &r));
    struct re2_rsp_decode d = {
        .width = 240, .k = {0xb375, 0x5b69, 0x2c0d, 0xe2d1, 0x950b}, .zero = 0x4000, .max = 0x7f80};
    uint8_t y[480], cb[240], cr[240], row0[960], row1[960];
    memset(y, 100, sizeof(y));
    memset(cb, 128, sizeof(cb));
    memset(cr, 128, sizeof(cr));
    re2_rsp_decode_pair(&d, y, cb, cr, row0, row1);
    for (unsigned i = 0; i < 240; i++) {
        for (unsigned k = 0; k < 3; k++)
            assert(row0[i * 4 + k] == 58 && row1[i * 4 + k] == 58);
        assert(row0[i * 4 + 3] == (i < 28 ? 128 : 0));
        assert(row1[i * 4 + 3] == 0);
    }
    memset(data, 0, sizeof(data));
    word(data, 16, 240);
    word(data, 20, 120);
    word(data, 24, 2);
    word(data, 28, 60);
    word(data, 32, 0x060013bf);
    word(data, 36, 0x840);
    assert(re2_rsp_decode_read(data, &d));
    word(data, 16, 256);
    assert(!re2_rsp_decode_read(data, &d));
    uint8_t source[8], destination[8];
    for (unsigned a = 0; a < 256; a++)
        for (unsigned b = 0; b < 256; b++) {
            memset(source, a, sizeof(source));
            memset(destination, b, sizeof(destination));
            re2_rsp_fill_row(source, destination, sizeof(source));
            for (unsigned i = 0; i < sizeof(source); i++)
                assert(destination[i] == (a + b) / 2);
        }
    struct re2_rsp_fill f;
    memset(data, 0, sizeof(data));
    word(data, 8, 0x780);
    word(data, 12, 2);
    word(data, 16, 60);
    word(data, 24, 0x060003bf);
    word(data, 28, 0x840);
    word(data, 36, 0x003e0001);
    word(data, 40, 0x80000800);
    assert(re2_rsp_fill_read(data, &f));
    assert(f.rows == 2 && f.len == 960 && f.skip == 96);
    word(data, 12, 0xffffffff);
    assert(!re2_rsp_fill_read(data, &f));
    puts("RE2 resize, movie color/alpha, all byte blend pairs and malformed parameters passed");
}
