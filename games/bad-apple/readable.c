/*
 * Bad Apple!!: readable reconstruction of recomp_s0_80001000.
 *
 * REFERENCE ONLY. This file is not included in any build or linked into the
 * demo. The generated cpu-00-010000.c remains the executable implementation.
 *
 * Derived from the generated C for ROM SHA-1:
 *   638e81902bbd57114de3eba39fba3aabd322c6df
 * Original guest instructions: 0x80001000 through 0x800012e8.
 *
 * The algorithm, buffer addresses and device-write order are preserved for
 * this valid ROM. Register bookkeeping, instruction traces, scheduler
 * checkpoints, delay slots and the word-prefetch optimization are omitted.
 * Consequently this is an explanatory translation, not a drop-in replacement
 * with identical scheduling, diagnostics or malformed-input behavior.
 *
 * Addresses below are N64 guest addresses, never native host pointers. The
 * recomp_load/store helpers express guest byte order and device accesses.
 * No ROM content, video, audio or RDP command data is included here.
 */
#include "recomp.h"

#define VI_BASE          UINT32_C(0xa4400000)
#define AI_BASE          UINT32_C(0xa4500000)
#define PI_BASE          UINT32_C(0xa4600000)
#define DPC_BASE         UINT32_C(0xa4100000)
#define PIF_BOOT_COMMAND UINT32_C(0xbfc007fc)

#define FRAMEBUFFER      UINT32_C(0xa0100000)
#define RLE_BUFFER       UINT32_C(0x80300000)
#define GRAYSCALE_BUFFER UINT32_C(0x80380000)
#define AUDIO_BUFFER     UINT32_C(0x800016d8)
#define AUDIO_CART_START UINT32_C(0x100016d8)
#define VIDEO_ROM_START  UINT32_C(0xb126eed8)
#define RDP_LIST_START   UINT32_C(0x800012f0)
#define RDP_LIST_END     UINT32_C(0x800016d8)

enum {
    FRAME_PIXELS = 320 * 240,
    MOVIE_FRAMES = 6572,
    AUDIO_BYTES_PER_FRAME = 2940
};

/* 0x80001000..0x800010b8: one-time hardware setup. */
static void configure_video_and_audio(Runtime *runtime) {
    recomp_store32(runtime, PIF_BOOT_COMMAND, 8); /* Finish the boot handshake. */

    recomp_store32(runtime, VI_BASE + 0x00, 0x00000203); /* Status: RGBA32. */
    recomp_store32(runtime, VI_BASE + 0x04, FRAMEBUFFER);
    recomp_store32(runtime, VI_BASE + 0x08, 320);        /* Image width. */
    recomp_store32(runtime, VI_BASE + 0x0c, 0x00000200); /* Interrupt line. */
    recomp_store32(runtime, VI_BASE + 0x10, 0);          /* Clear interrupt. */
    recomp_store32(runtime, VI_BASE + 0x14, 0x03e52239); /* Burst timing. */
    recomp_store32(runtime, VI_BASE + 0x18, 0x0000020d); /* Vertical sync. */
    recomp_store32(runtime, VI_BASE + 0x1c, 0x00000c15); /* Horizontal sync. */
    recomp_store32(runtime, VI_BASE + 0x20, 0x0c150c15); /* Horizontal leap. */
    recomp_store32(runtime, VI_BASE + 0x24, 0x006c02ec); /* Horizontal range. */
    recomp_store32(runtime, VI_BASE + 0x28, 0x002501ff); /* Vertical range. */
    recomp_store32(runtime, VI_BASE + 0x2c, 0x000e0204); /* Vertical burst. */
    recomp_store32(runtime, VI_BASE + 0x30, 0x00000200); /* X scale. */
    recomp_store32(runtime, VI_BASE + 0x34, 0x00000400); /* Y scale. */

    recomp_store32(runtime, AI_BASE + 0x08, 1);     /* Enable audio DMA. */
    recomp_store32(runtime, AI_BASE + 0x14, 15);    /* 16-bit samples. */
    recomp_store32(runtime, AI_BASE + 0x10, 0x89e); /* Clock divisor minus one. */
}

/* 0x800010d8..0x800011dc: expand one LZSS block into the RLE buffer.
 *
 * Header: one format byte (ignored by the original), then the decompressed
 * size as three little-endian bytes. Each flag byte describes eight tokens,
 * most significant bit first: 0 = literal byte, 1 = two-byte backreference.
 * The returned guest ROM address is the next block's four-byte boundary.
 */
static uint32_t decompress_lzss_frame(Runtime *runtime, uint32_t source) {
    uint32_t output_size =
        (uint32_t)recomp_load8(runtime, source + 1) |
        ((uint32_t)recomp_load8(runtime, source + 2) << 8) |
        ((uint32_t)recomp_load8(runtime, source + 3) << 16);
    source += 4;

    uint32_t output = RLE_BUFFER;
    uint32_t output_end = output + output_size;

    while (output != output_end) {
        uint8_t flags = recomp_load8(runtime, source++);
        for (unsigned mask = 0x80; mask != 0 && output != output_end; mask >>= 1) {
            uint8_t first = recomp_load8(runtime, source++);
            if ((flags & mask) == 0) {
                recomp_store8(runtime, output++, first);
            } else {
                uint8_t second = recomp_load8(runtime, source++);
                unsigned length = (first >> 4) + 3;
                unsigned distance = (((unsigned)first & 0x0f) << 8) + second + 1;
                uint32_t previous = output - distance;

                /* Copy forward one byte at a time: a match may reuse bytes
                 * produced by this very match. memcpy/memmove do not express
                 * this expanding repetition when length exceeds distance. */
                for (unsigned i = 0; i < length; i++) {
                    uint8_t value = recomp_load8(runtime, previous++);
                    recomp_store8(runtime, output++, value);
                }
            }
        }
    }

    /* The original reads ahead in 32-bit words and backs up one word when
     * the final token ends exactly at a word boundary. This is its logical
     * next-block position after discarding any padding/prefetched bytes. */
    return (source + 3) & ~UINT32_C(3);
}

/* 0x800011e0..0x80001214: reuse one RAM buffer for the next PCM chunk. */
static void queue_audio_frame(Runtime *runtime, uint32_t cartridge_address) {
    /* The original LB reads the high byte of big-endian AI_STATUS.
     * Its 0x40 mask is therefore the full register's 0x40000000 busy bit. */
    while (recomp_load8(runtime, AI_BASE + 0x0c) & 0x40) {
        /* Wait until audio DMA releases the buffer. */
    }

    recomp_store32(runtime, PI_BASE + 0x00, AUDIO_BUFFER);
    recomp_store32(runtime, PI_BASE + 0x04, cartridge_address);
    recomp_store32(runtime, PI_BASE + 0x0c, AUDIO_BYTES_PER_FRAME - 1);

    /* Preserve the original order: it does not poll PI completion here.
     * The native demo adapter completes this cartridge-to-RAM DMA inline. */
    recomp_store32(runtime, AI_BASE + 0x00, AUDIO_BUFFER);
    recomp_store32(runtime, AI_BASE + 0x04, AUDIO_BYTES_PER_FRAME - 1);

    /* PI length is encoded as count minus one: 2939 requests 2940 bytes.
     * AI instead masks the same value to an eight-byte multiple: 2936 bytes
     * (734 stereo sample frames) play; the ROM cursor still advances 2940. */
}

/* 0x80001218..0x8000128c: expand RLE into 320 x 240 intensity bytes.
 * The four-byte RLE header is skipped; image size is fixed by the program.
 */
static void decompress_rle_pixels(Runtime *runtime) {
    uint32_t source = RLE_BUFFER + 4;
    uint32_t output = GRAYSCALE_BUFFER;
    uint32_t output_end = GRAYSCALE_BUFFER + FRAME_PIXELS;

    while (output != output_end) {
        uint8_t token = recomp_load8(runtime, source++);
        unsigned count = token & 0x7f;

        if (token & 0x80) {
            /* Repeated run: one following byte, repeated 3..130 times. */
            count += 3;
            uint8_t intensity = recomp_load8(runtime, source++);
            for (unsigned i = 0; i < count; i++)
                recomp_store8(runtime, output++, intensity);
        } else {
            /* Literal run: copy the next 1..128 bytes. */
            count += 1;
            for (unsigned i = 0; i < count; i++) {
                uint8_t intensity = recomp_load8(runtime, source++);
                recomp_store8(runtime, output++, intensity);
            }
        }
    }
}

/* 0x80001290..0x800012d8: make the image visible and submit its drawing list. */
static void present_frame(Runtime *runtime) {
    /* Original hardware: Index Writeback Invalidate D for all 512 cache
     * indices, addressed at 0x80000000..0x80001ff0 in steps of 16.
     * This is already a no-op in the generated C: host RAM is coherent. */

    while (recomp_load32(runtime, VI_BASE + 0x10) != 480) {
        /* Wait for scanline 480. The native adapter paces this at 30 FPS. */
    }

    /* The list already loaded from the ROM draws the grayscale image in
     * 24 strips, each 10 rows high, into the RGBA32 framebuffer. */
    recomp_store32(runtime, DPC_BASE + 0x00, RDP_LIST_START);
    recomp_store32(runtime, DPC_BASE + 0x04, RDP_LIST_END);
}

/* Structured equivalent of the whole entry function. Reference only. */
static void bad_apple_movie_reference(Runtime *runtime) {
    configure_video_and_audio(runtime);

    for (;;) {
        uint32_t video_cursor = VIDEO_ROM_START;
        uint32_t audio_cursor = AUDIO_CART_START;

        /* The MIPS counter starts at 0x19ab (6571) and its branch tests the
         * old value before decrementing in the delay slot: 6572 frames. */
        for (unsigned frame = 0; frame < MOVIE_FRAMES; frame++) {
            video_cursor = decompress_lzss_frame(runtime, video_cursor);
            queue_audio_frame(runtime, audio_cursor);
            audio_cursor += AUDIO_BYTES_PER_FRAME;
            decompress_rle_pixels(runtime);
            present_frame(runtime);
        }
        /* Restart both streams; hardware initialization is not repeated. */
    }
}
