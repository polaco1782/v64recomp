#include "fast3d.h"
#include <string.h>

enum { F3D_DL = 0x06, F3D_ENDDL = 0xb8, F3D_MOVEWORD = 0xbc, F3D_MW_SEGMENT = 0x06, F3D_STACK_SIZE = 32 };

static uint32_t physical_span(Runtime *rt, uint32_t address, size_t bytes) {
    /* Accept only physical RDRAM and its direct cached/uncached aliases.
     * A display-list pointer is not a CPU TLB address or a device access. */
    uint32_t physical = address;
    if ((address & UINT32_C(0xe0000000)) == UINT32_C(0x80000000) ||
        (address & UINT32_C(0xe0000000)) == UINT32_C(0xa0000000))
        physical &= UINT32_C(0x1fffffff);
    if (physical >= rt->rdram_size || bytes > rt->rdram_size - physical)
        recomp_fail("Fast3D address outside RDRAM", address);
    return physical;
}

uint32_t recomp_fast3d_address(Runtime *rt, const RecompFast3D *state, uint32_t address, size_t bytes) {
    if (address & UINT32_C(0x80000000))
        return physical_span(rt, address, bytes);
    unsigned segment = address >> 24;
    if (segment >= 16)
        recomp_fail("invalid Fast3D segment", address);
    uint64_t physical = (uint64_t)state->segments[segment] + (address & UINT32_C(0xffffff));
    if (physical > UINT32_MAX)
        recomp_fail("Fast3D segment address overflow", address);
    return physical_span(rt, (uint32_t)physical, bytes);
}

static uint32_t word(Runtime *rt, uint32_t physical) {
    /* Runtime RAM uses native 32-bit words on little-endian hosts, and guest
     * big-endian bytes otherwise. memcpy also avoids alignment/aliasing UB. */
    uint32_t value;
#if RECOMP_LITTLE_ENDIAN_HOST
    memcpy(&value, rt->rdram + physical, sizeof(value));
#else
    const uint8_t *p = rt->rdram + physical;
    value = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
#endif
    return value;
}

void recomp_fast3d_walk(Runtime *rt, RecompFast3D *state, uint32_t root, uint32_t max_commands, RecompFast3DSink sink,
                        void *userdata) {
    if (!sink || !max_commands || !rt->rdram)
        recomp_fail("invalid Fast3D traversal configuration", root);
    uint32_t returns[F3D_STACK_SIZE], depth = 0;
    uint32_t pc = physical_span(rt, root, 8);
    for (uint32_t steps = 0; steps < max_commands; steps++) {
        if (pc & 7)
            recomp_fail("unaligned Fast3D display list", pc);
        physical_span(rt, pc, 8);
        RecompFast3DCommand cmd = {pc, word(rt, pc), word(rt, pc + 4)};
        unsigned opcode = cmd.w0 >> 24;
        uint32_t next = pc + 8;
        if (opcode == F3D_MOVEWORD && (cmd.w0 & 255) == F3D_MW_SEGMENT) {
            uint32_t offset = (cmd.w0 >> 8) & 65535;
            if ((offset & 3) || offset >= 16 * 4)
                recomp_fail("invalid Fast3D segment register", pc);
            state->segments[offset / 4] = physical_span(rt, cmd.w1, 1);
        }
        if (!sink(rt, userdata, state, &cmd))
            recomp_fail("Fast3D sink rejected command", pc);
        if (opcode == F3D_DL) {
            uint32_t branch = (cmd.w0 >> 16) & 255;
            if (branch > 1)
                recomp_fail("invalid Fast3D display-list branch", pc);
            if (!branch) {
                if (depth == F3D_STACK_SIZE)
                    recomp_fail("Fast3D display-list stack exhausted", pc);
                returns[depth++] = next;
            }
            next = recomp_fast3d_address(rt, state, cmd.w1, 8);
        } else if (opcode == F3D_ENDDL) {
            if (!depth)
                return;
            next = returns[--depth];
        }
        pc = next;
    }
    recomp_fail("Fast3D command budget exhausted", pc);
}
