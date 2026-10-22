#include "cheats.h"
#include <stdio.h>

static bool condition(unsigned type) {
    return (type >= 0xd0 && type <= 0xd3) || (type >= 0xd8 && type <= 0xdb);
}
static bool write_code(unsigned type) {
    return type == 0x80 || type == 0x81 || type == 0xa0 || type == 0xa1 || type == 0x88 || type == 0x89 ||
           type == 0xa8 || type == 0xa9;
}
static bool button_code(unsigned type) {
    return type == 0x88 || type == 0x89 || type == 0xa8 || type == 0xa9 || type >= 0xd8;
}
static unsigned width(unsigned type) {
    if (condition(type))
        return type == 0xd1 || type == 0xd3 || type == 0xd9 || type == 0xda ? 2 : 1;
    return type & 1 ? 2 : 1;
}
static bool reject(char *reason, size_t size, const char *message, uint32_t address) {
    if (size)
        snprintf(reason, size, "%s (%08X)", message, address);
    return false;
}
static bool valid_address(const Runtime *rt, uint32_t address, unsigned bytes, bool write, char *reason, size_t size) {
    uint32_t physical = address & 0xffffff;
    if (physical % bytes || physical >= rt->rdram_size || bytes > rt->rdram_size - physical)
        return reject(reason, size, "Address is outside RDRAM or misaligned", address);
    if (!write)
        return true;
    /* Rewriting a MIPS instruction cannot rewrite the compiled host function.
     * Reject overlaps we can identify without invoking a game's pager. */
    for (size_t i = 0; i < rt->function_count; i++) {
        const RecompFunctionEntry *f = &rt->functions[i];
        uint32_t a = f->address;
        if (a >= 0x80000000 && a < 0xc0000000) {
            a &= 0x1fffffff;
            if ((uint64_t)physical < (uint64_t)a + f->size && (uint64_t)a < (uint64_t)physical + bytes)
                return reject(reason, size, "Instruction patches cannot change recompiled code", address);
        }
    }
    return true;
}
bool recomp_cheats_validate(const Runtime *rt, const RecompCheatCode *codes, size_t count, char *reason, size_t size) {
    if (size)
        reason[0] = 0;
    if (!count || count > 4096)
        return reject(reason, size, "Empty cheat or too many code lines", 0);
    size_t writes = 0;
    bool pending_condition = false;
    for (size_t i = 0; i < count; i++) {
        RecompCheatCode code = codes[i];
        unsigned type = code.address >> 24;
        if (condition(type)) {
            if (!valid_address(rt, code.address, width(type), false, reason, size))
                return false;
            pending_condition = true;
            continue;
        }
        unsigned repeat = 1, stride = 0;
        if ((code.address & 0xffff0000) == 0x50000000) {
            repeat = (code.address >> 8) & 0xff;
            stride = code.address & 0xff;
            if (!repeat || ++i == count)
                return reject(reason, size, "Invalid repeat code", code.address);
            code = codes[i];
            type = code.address >> 24;
        }
        if (!write_code(type))
            return reject(reason, size, "Unsupported GameShark type (boot/encrypted codes are unavailable)",
                          code.address);
        if ((writes += repeat) > 16384)
            return reject(reason, size, "Cheat expands to too many writes", code.address);
        for (unsigned n = 0; n < repeat; n++) {
            uint64_t target = (uint64_t)(code.address & 0xffffff) + n * stride;
            if (target > 0xffffff ||
                !valid_address(rt, (code.address & 0xff000000) | (uint32_t)target, width(type), true, reason, size))
                return false;
        }
        pending_condition = false;
    }
    if (pending_condition)
        return reject(reason, size, "Condition has no following write", codes[count - 1].address);
    return true;
}
static unsigned read_ram(const Runtime *rt, uint32_t p, unsigned bytes) {
    unsigned value = rt->rdram[p ^ 3];
    if (bytes == 2)
        value = (value << 8) | rt->rdram[(p + 1) ^ 3];
    return value;
}
void recomp_cheats_apply(Runtime *rt, const RecompCheatCode *codes, size_t count, bool gs_button) {
    bool passed = true;
    for (size_t i = 0; i < count; i++) {
        RecompCheatCode code = codes[i];
        unsigned type = code.address >> 24, bytes = width(type);
        uint32_t p = code.address & 0xffffff;
        if (condition(type)) {
            if (p >= rt->rdram_size || bytes > rt->rdram_size - p)
                return;
            unsigned expected = bytes == 1 ? code.value & 0xff : code.value;
            bool equal = read_ram(rt, p, bytes) == expected;
            bool negate = type == 0xd2 || type == 0xd3 || type == 0xda || type == 0xdb;
            passed = passed && (equal != negate) && (!button_code(type) || gs_button);
            continue;
        }
        unsigned repeat = 1, stride = 0, increment = 0;
        if ((code.address & 0xffff0000) == 0x50000000) {
            repeat = (code.address >> 8) & 0xff;
            stride = code.address & 0xff;
            increment = code.value;
            if (++i == count)
                return;
            code = codes[i];
            type = code.address >> 24;
            bytes = width(type);
            p = code.address & 0xffffff;
        }
        if (!write_code(type))
            return;
        if (passed && (!button_code(type) || gs_button)) {
            for (unsigned n = 0; n < repeat; n++) {
                uint32_t target = p + n * stride;
                if (target >= rt->rdram_size || bytes > rt->rdram_size - target)
                    return;
                unsigned value = (code.value + n * increment) & 0xffff;
                rt->rdram[target ^ 3] = (uint8_t)(bytes == 2 ? value >> 8 : value);
                if (bytes == 2)
                    rt->rdram[(target + 1) ^ 3] = (uint8_t)value;
            }
        }
        passed = true;
    }
}
