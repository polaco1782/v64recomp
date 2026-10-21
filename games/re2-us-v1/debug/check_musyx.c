/* Native MusyX v1 fixtures; no ROM assets. Build with ASan/UBSan:
 * clang -std=c11 -O1 -g -fsanitize=address,undefined -fno-strict-aliasing \
 *   -Ithird_party/rsp-hle games/re2-us-v1/debug/check_musyx.c \
 *   third_party/rsp-hle/musyx.c \
 *   third_party/rsp-hle/audio.c third_party/rsp-hle/memory.c -o /tmp/check-musyx
 * /tmp/check-musyx [word-swapped-rdram task-list-address subframe-count]
 */
#include "hle_internal.h"
#include "hle_external.h"
#include "memory.h"
#include <assert.h>
#include <stdbool.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static _Alignas(16) unsigned char ram[0x800000], dmem[4096];
static struct hle_t hle;
static jmp_buf failure;
static bool expect_failure, completed;

void HleVerboseMessage(void *opaque, const char *message, ...) { (void)opaque; (void)message; }
void HleWarnMessage(void *opaque, const char *message, ...) {
    (void)opaque;
    if (expect_failure) longjmp(failure, 1);
    fprintf(stderr, "Unexpected MusyX failure: %s\n", message);
    abort();
}
void HleMusyxValidateRam(void *opaque, uint32_t address, size_t bytes, unsigned alignment) {
    uint32_t physical = address;
    if (address >= 0x80000000 && address < 0xc0000000) physical &= 0x1fffffff;
    if (physical != (address & 0xffffff) || physical >= sizeof(ram) ||
            bytes > sizeof(ram) - physical || address % alignment)
        HleWarnMessage(opaque, "MusyX guest range/alignment");
}
void rsp_break(struct hle_t *context, unsigned bits) {
    assert(context == &hle && bits == SP_STATUS_TASKDONE);
    completed = true;
}
static void put16(uint32_t address, uint16_t value) { *dram_u16(&hle, address) = value; }
static void put32(uint32_t address, uint32_t value) { *dram_u32(&hle, address) = value; }
static void reset(void) {
    memset(ram, 0, sizeof(ram)); memset(dmem, 0, sizeof(dmem));
    completed = false;
    *dmem_u32(&hle, TASK_DATA_PTR) = 0x80001000;
    *dmem_u32(&hle, TASK_DATA_SIZE) = 1;
    put32(0x1008, 0x80004000); /* state */
    put32(0x1054, 0x80005000); /* first voice's output terminator */
}
static void run(bool reject) {
    completed = false;
    expect_failure = reject;
    if (!setjmp(failure)) {
        musyx_v1_task(&hle);
        assert(!reject && completed);
    } else assert(reject && !completed);
    expect_failure = false;
}
int main(int argc, char **argv) {
    hle.dram = ram; hle.dmem = dmem;
    if (argc == 4) {
        FILE *in = fopen(argv[1], "rb"); assert(in);
        assert(fread(ram, 1, sizeof(ram), in) == sizeof(ram));
        assert(fgetc(in) == EOF); fclose(in);
        *dmem_u32(&hle, TASK_DATA_PTR) = (uint32_t)strtoul(argv[2], NULL, 0);
        *dmem_u32(&hle, TASK_DATA_SIZE) = (uint32_t)strtoul(argv[3], NULL, 0);
        run(false); puts("Captured MusyX task completed"); return 0;
    }
    assert(argc == 1);
    reset(); run(false);
    for (unsigned i = 0; i < 192; i++) assert(*dram_u32(&hle, 0x5000 + i * 4) == 0);
    /* Constant base volumes decay by 0xf850/65536, then interleave L/R. */
    reset(); put16(0x4108, 1000); put16(0x4102, 0xffff); put16(0x410a, (uint16_t)-1000); run(false);
    for (unsigned i = 0; i < 192; i++)
        assert(*dram_u32(&hle, 0x5000 + i * 4) == ((uint32_t)969 << 16 | (uint16_t)-970));
    /* Constant PCM with unity pitch and opposite stereo envelopes. */
    for (unsigned end = 256; end <= 0x4000; end *= 64) {
        reset(); put32(0x1034, 0x80006000); put16(0x103c, 512);
        put16(0x1050, 256); put16(0x1032, 0x1000); put16(0x1058, end);
        put16(0x105a, 0xffff); /* Unused restart sentinel must not reject valid taps. */
        put32(0x1010, 0x7fff0000); put32(0x1014, 0x80000000);
        for (unsigned i = 0; i < 256; i++) put16(0x6000 + i * 2, 1000);
        run(false);
        for (unsigned i = 0; i < 192; i++)
            assert(*dram_u32(&hle, 0x5000 + i * 4) == ((uint32_t)997 << 16 | (uint16_t)-998));
        put16(0x1058, 0); run(true); /* A taken restart outside the buffer still fails. */
    }
    /* Eight zero ADPCM frames (four 40-byte frame pairs). */
    reset(); put32(0x1034, 0x80006000); put16(0x103c, 160);
    *dram_u8(&hle, 0x104c) = 8; put32(0x1050, 0x80007000);
    put16(0x1032, 0x1000); put16(0x1058, 256); run(false);
    reset(); *dmem_u32(&hle, TASK_DATA_SIZE) = 0; run(true);
    reset(); put32(0x1008, 0x807fff00); run(true);
    reset(); put32(0x1054, 0x807ffffc); run(true);
    reset(); put32(0x1034, 0x80006000); put16(0x103c, 1026); put16(0x1050, 513); run(true);
    reset(); put32(0x1034, 0x80006000); put16(0x103c, 1);
    *dram_u8(&hle, 0x104c) = 1; put32(0x1050, 0x80007000); run(true);
    reset(); put32(0x100c, 0x80006000); put32(0x6000, 0x80007000);
    put32(0x6004, 192); put16(0x6008, 9); run(true);
    puts("MusyX silence, stereo base volume, PCM, ADPCM and malformed spans passed");
}
