#ifndef RECOMP_RSP_H
#define RECOMP_RSP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* RSP state uses guest-order DMEM/IMEM bytes and native-order scalar/vector
 * elements. RDRAM keeps the runtime's native-word layout. Generated C contains
 * the instruction implementations; IMEM DMA only binds verified translations. */
typedef struct RecompRspState RecompRspState;
typedef struct RecompRspCode {
    const char *name;
    const uint32_t *words;
    const uint16_t *entries;
    size_t count;
} RecompRspCode;
typedef struct RecompRspProgram {
    const RecompRspCode *images;
    size_t image_count;
    void (*run)(RecompRspState *);
    const uint8_t *block_lengths;
    size_t entry_count;
} RecompRspProgram;
typedef struct RecompRspWrite {
    uint32_t address, bytes;
} RecompRspWrite;
struct RecompRspState {
    uint32_t gpr[32], pc, next_pc, instruction_pc;
    uint16_t v[32][8], vco, vcc;
    uint8_t vce;
    int64_t accumulator[8];
    uint32_t div_input, div_output;
    bool div_pending;
    uint8_t dmem[4096], imem[4096];
    uint16_t native[1024];
    uint8_t blocks[1024]; /* Currently verified contiguous instructions per entry. */
    uint32_t cp0[16];
    uint8_t *ram;
    size_t ram_size;
    const RecompRspProgram *program;
    uint64_t instructions, instruction_limit;
    bool halted;
    const char *error;
    uint32_t error_address, error_word;
    uint32_t *rdp;
    size_t rdp_count, rdp_capacity;
    RecompRspWrite *writes;
    size_t write_count, write_capacity;
};

void recomp_rsp_init(RecompRspState *, uint8_t *ram, size_t ram_size, const RecompRspProgram *);
void recomp_rsp_destroy(RecompRspState *);
bool recomp_rsp_start(RecompRspState *, const uint32_t task[16]);
void recomp_rsp_fault(RecompRspState *, const char *, uint32_t address, uint32_t word);
uint32_t recomp_rsp_load(RecompRspState *, uint32_t address, unsigned bytes);
void recomp_rsp_store(RecompRspState *, uint32_t address, uint32_t value, unsigned bytes);
uint32_t recomp_rsp_cp0_read(RecompRspState *, unsigned reg);
void recomp_rsp_cp0_write(RecompRspState *, unsigned reg, uint32_t value);
uint32_t recomp_rsp_mfc2(RecompRspState *, unsigned reg, unsigned element);
void recomp_rsp_mtc2(RecompRspState *, unsigned reg, unsigned element, uint32_t value);
uint32_t recomp_rsp_cfc2(RecompRspState *, unsigned reg);
void recomp_rsp_ctc2(RecompRspState *, unsigned reg, uint32_t value);

#define RECOMP_RSP_VECTOR_OPS(X)                                                                                       \
    X(vmulf)                                                                                                           \
    X(vmulu)                                                                                                           \
    X(vrndp)                                                                                                           \
    X(vmulq)                                                                                                           \
    X(vmudl)                                                                                                           \
    X(vmudm)                                                                                                           \
    X(vmudn)                                                                                                           \
    X(vmudh)                                                                                                           \
    X(vmacf)                                                                                                           \
    X(vmacu)                                                                                                           \
    X(vrndn)                                                                                                           \
    X(vmacq) X(vmadl) X(vmadm) X(vmadn) X(vmadh) X(vadd) X(vsub) X(vabs) X(vaddc) X(vsubc) X(vsar) X(vlt) X(veq)       \
        X(vne) X(vge) X(vcl) X(vch) X(vcr) X(vmrg) X(vand) X(vnand) X(vor) X(vnor) X(vxor) X(vnxor) X(vrcp) X(vrcpl)   \
            X(vrcph) X(vmov) X(vrsq) X(vrsql) X(vrsqh)
#define RECOMP_RSP_DECLARE(name) void recomp_rsp_##name(RecompRspState *, unsigned, unsigned, unsigned, unsigned);
RECOMP_RSP_VECTOR_OPS(RECOMP_RSP_DECLARE)
#undef RECOMP_RSP_DECLARE

#define RECOMP_RSP_VECTOR_MEMORY(X) X(bv) X(sv) X(lv) X(dv) X(qv) X(rv) X(pv) X(uv) X(hv) X(fv) X(wv) X(tv)
#define RECOMP_RSP_DECLARE_MEMORY(name)                                                                                \
    void recomp_rsp_l##name(RecompRspState *, unsigned reg, unsigned element, uint32_t address);                       \
    void recomp_rsp_s##name(RecompRspState *, unsigned reg, unsigned element, uint32_t address);
RECOMP_RSP_VECTOR_MEMORY(RECOMP_RSP_DECLARE_MEMORY)
#undef RECOMP_RSP_DECLARE_MEMORY
#ifdef __cplusplus
}
#endif
#endif
