#include "rsp.h"
#include "recomp.h"
#include <stdlib.h>
#include <string.h>

void recomp_rsp_vector_tables_init(void);

void recomp_rsp_fault(RecompRspState *s, const char *message, uint32_t address, uint32_t word) {
    if (!s->error) {
        s->error = message;
        s->error_address = address;
        s->error_word = word;
    }
    s->halted = true;
}
static bool ram_span(RecompRspState *s, uint32_t address, size_t bytes) {
    if (address > s->ram_size || bytes > s->ram_size - address) {
        recomp_rsp_fault(s, "RSP DMA outside RDRAM", address, (uint32_t)bytes);
        return false;
    }
    return true;
}
static uint8_t ram_byte(const RecompRspState *s, uint32_t address) {
#if RECOMP_LITTLE_ENDIAN_HOST
    return s->ram[address ^ 3];
#else
    return s->ram[address];
#endif
}
static void ram_store(RecompRspState *s, uint32_t address, uint8_t value) {
#if RECOMP_LITTLE_ENDIAN_HOST
    s->ram[address ^ 3] = value;
#else
    s->ram[address] = value;
#endif
}
static uint32_t ram_word(const RecompRspState *s, uint32_t address) {
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; i++)
        value = (value << 8) | ram_byte(s, address + i);
    return value;
}
uint32_t recomp_rsp_load(RecompRspState *s, uint32_t address, unsigned bytes) {
    uint32_t value = 0;
    for (unsigned i = 0; i < bytes; i++)
        value = (value << 8) | s->dmem[(address + i) & 4095];
    return value;
}
void recomp_rsp_store(RecompRspState *s, uint32_t address, uint32_t value, unsigned bytes) {
    for (unsigned i = 0; i < bytes; i++)
        s->dmem[(address + i) & 4095] = (uint8_t)(value >> ((bytes - 1 - i) * 8));
}
/* A partial/overlapping DMA can truncate an earlier block. Rebuild the small
 * IMEM map after binding so a compiled fall-through never crosses changed or
 * untranslated words. Blocks stop at the IMEM wrap boundary. */
static void bind_blocks(RecompRspState *s) {
    for (unsigned i = 1024; i-- > 0;) {
        uint16_t entry = s->native[i];
        unsigned length = 0;
        if (entry < s->program->entry_count) {
            length = 1;
            unsigned remaining = s->program->block_lengths[entry];
            if (remaining > 1 && i + 1 < 1024 && s->native[i + 1] == entry + 1) {
                length += s->blocks[i + 1];
                if (length > remaining)
                    length = remaining;
            }
        }
        s->blocks[i] = (uint8_t)length;
    }
}
static bool bind_imem(RecompRspState *s, uint32_t dram, uint32_t target, unsigned bytes) {
    for (size_t image = 0; image < s->program->image_count; image++) {
        const RecompRspCode *code = &s->program->images[image];
        unsigned words = bytes / 4;
        if (words > code->count)
            continue;
        for (size_t offset = 0; offset <= code->count - words; offset++) {
            if (ram_word(s, dram) != code->words[offset])
                continue;
            unsigned i = 1;
            while (i < words && ram_word(s, dram + i * 4) == code->words[offset + i])
                i++;
            if (i != words)
                continue;
            for (i = 0; i < words; i++)
                s->native[((target + i * 4) & 4095) / 4] = code->entries[offset + i];
            for (i = 0; i < bytes; i++)
                s->imem[(target + i) & 4095] = ram_byte(s, dram + i);
            bind_blocks(s);
            return true;
        }
    }
    recomp_rsp_fault(s, "RSP IMEM DMA has no verified native translation", dram, bytes);
    return false;
}
static bool journal(RecompRspState *s, uint32_t address, unsigned bytes) {
    if (s->write_count) {
        RecompRspWrite *last = &s->writes[s->write_count - 1];
        if (address <= last->address + last->bytes && address + bytes >= last->address) {
            uint32_t end =
                address + bytes > last->address + last->bytes ? address + bytes : last->address + last->bytes;
            if (address < last->address)
                last->address = address;
            last->bytes = end - last->address;
            return true;
        }
    }
    if (s->write_count == s->write_capacity) {
        size_t capacity = s->write_capacity ? s->write_capacity * 2 : 64;
        RecompRspWrite *writes = capacity <= 65536 ? realloc(s->writes, capacity * sizeof(*writes)) : NULL;
        if (!writes) {
            recomp_rsp_fault(s, "RSP write journal exhausted", address, bytes);
            return false;
        }
        s->writes = writes;
        s->write_capacity = capacity;
    }
    s->writes[s->write_count++] = (RecompRspWrite){address, bytes};
    return true;
}
static void dma(RecompRspState *s, uint32_t value, bool write) {
    unsigned bytes = (value & 0xff8) + 8, rows = ((value >> 12) & 255) + 1, skip = value >> 20;
    uint32_t mem = s->cp0[0] & 0x1ff8, dram = s->cp0[1] & 0xfffff8;
    bool imem = (mem & 4096) != 0;
    for (unsigned row = 0; row < rows && !s->error; row++) {
        if (!ram_span(s, dram, bytes))
            return;
        if (write) {
            if (!journal(s, dram, bytes))
                return;
            const uint8_t *source = imem ? s->imem : s->dmem;
            for (unsigned i = 0; i < bytes; i++)
                ram_store(s, dram + i, source[(mem + i) & 4095]);
        } else if (imem) {
            if (!bind_imem(s, dram, mem, bytes))
                return;
        } else
            for (unsigned i = 0; i < bytes; i++)
                s->dmem[(mem + i) & 4095] = ram_byte(s, dram + i);
        mem = (mem & 4096) | ((mem + bytes) & 4095);
        dram += bytes + (row + 1 < rows ? skip : 0);
    }
    s->cp0[0] = mem;
    s->cp0[1] = dram;
    s->cp0[write ? 3 : 2] = 0xff8;
}
static void rdp_append(RecompRspState *s, uint32_t end) {
    bool xbus = (s->cp0[11] & 1) != 0;
    uint32_t start = s->cp0[10], bytes;
    if ((start | end) & 7) {
        recomp_rsp_fault(s, "RSP submitted an unaligned RDP buffer", start, end);
        return;
    }
    if (xbus)
        bytes = (end - start) & 4095;
    else {
        if (end < start || !ram_span(s, start, end - start)) {
            recomp_rsp_fault(s, "RSP submitted an invalid RDP buffer", start, end);
            return;
        }
        bytes = end - start;
    }
    size_t count = bytes / 4, required = s->rdp_count + count;
    if (required > s->rdp_capacity) {
        size_t capacity = s->rdp_capacity ? s->rdp_capacity : 4096;
        while (capacity < required && capacity <= 4 * 1024 * 1024)
            capacity *= 2;
        uint32_t *words = capacity <= 4 * 1024 * 1024 ? realloc(s->rdp, capacity * sizeof(*words)) : NULL;
        if (!words) {
            recomp_rsp_fault(s, "RSP RDP output limit exceeded", start, bytes);
            return;
        }
        s->rdp = words;
        s->rdp_capacity = capacity;
    }
    for (size_t i = 0; i < count; i++)
        s->rdp[s->rdp_count++] =
            xbus ? recomp_rsp_load(s, start + (uint32_t)i * 4, 4) : ram_word(s, start + (uint32_t)i * 4);
    s->cp0[10] = end;
}
uint32_t recomp_rsp_cp0_read(RecompRspState *s, unsigned reg) {
    reg &= 15;
    if (reg == 5 || reg == 6 || reg >= 12)
        return 0;
    if (reg == 7) {
        uint32_t previous = s->cp0[7];
        s->cp0[7] = 1;
        return previous;
    }
    return s->cp0[reg];
}
void recomp_rsp_cp0_write(RecompRspState *s, unsigned reg, uint32_t value) {
    switch (reg & 15) {
        case 0:
            s->cp0[0] = value & 8191;
            break;
        case 1:
            s->cp0[1] = value & 0xffffff;
            break;
        case 2:
        case 3:
            dma(s, value, reg == 3);
            break;
        case 4:
            if (value & 1)
                s->cp0[4] &= ~1u;
            if (value & 2) {
                s->cp0[4] |= 1;
                s->halted = true;
            }
            if (value & 4)
                s->cp0[4] &= ~2u;
            for (unsigned i = 0; i < 10; i++) {
                unsigned status_bit = i + 5;
                unsigned command_bit = i * 2 + 5;
                if (value & (1u << command_bit))
                    s->cp0[4] &= ~(1u << status_bit);
                if (value & (1u << (command_bit + 1)))
                    s->cp0[4] |= 1u << status_bit;
            }
            break;
        case 7:
            s->cp0[7] = 0;
            break;
        case 8:
            s->cp0[8] = s->cp0[10] = value & 0xfffff8;
            break;
        case 9:
            s->cp0[9] = value & 0xfffff8;
            rdp_append(s, s->cp0[9]);
            break;
        case 11:
            for (unsigned i = 0; i < 3; i++) {
                if (value & (1u << (i * 2)))
                    s->cp0[11] &= ~(1u << i);
                if (value & (1u << (i * 2 + 1)))
                    s->cp0[11] |= 1u << i;
            }
            break;
        default:
            break; /* Read-only status/counters; writes have no task work. */
    }
}
void recomp_rsp_init(RecompRspState *s, uint8_t *ram, size_t ram_size, const RecompRspProgram *program) {
    *s = (RecompRspState){
        .ram = ram, .ram_size = ram_size, .program = program, .instruction_limit = UINT64_C(100000000)};
    recomp_rsp_vector_tables_init();
}
bool recomp_rsp_start(RecompRspState *s, const uint32_t task[16]) {
    s->halted = false;
    s->error = NULL;
    s->instructions = s->rdp_count = s->write_count = 0;
    s->pc = 0;
    s->next_pc = 4;
    memset(s->gpr, 0, sizeof(s->gpr));
    memset(s->cp0, 0, sizeof(s->cp0));
    memset(s->dmem, 0, sizeof(s->dmem));
    memset(s->native, 255, sizeof(s->native));
    memset(s->blocks, 0, sizeof(s->blocks));
    s->cp0[11] = 0x80; /* Synchronous command sink is ready and never busy. */
    for (unsigned i = 0; i < 16; i++)
        recomp_rsp_store(s, 0xfc0 + i * 4, task[i], 4);
    uint32_t boot = task[2] & 0x1fffffff;
    if (!s->program || !s->program->image_count || !s->program->run || !s->program->block_lengths ||
        !s->program->entry_count || s->program->entry_count >= UINT16_MAX ||
        task[3] != s->program->images[0].count * 4 || !ram_span(s, boot, task[3]) || !bind_imem(s, boot, 0, task[3])) {
        recomp_rsp_fault(s, "RSP bootstrap does not match the compiled program", boot, task[3]);
        return false;
    }
    s->program->run(s);
    return !s->error && s->halted;
}
void recomp_rsp_destroy(RecompRspState *s) {
    free(s->writes);
    free(s->rdp);
    *s = (RecompRspState){0};
}
