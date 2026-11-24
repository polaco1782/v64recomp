// On a generated transfer miss, preflight a bounded ROM-backed control-flow graph
// before executing any guest effects. Larger or unsupported graphs use live repair.
#include "runtime.h"
#include "hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifndef _WIN32
#include <unistd.h>
#endif

enum { MAX_WORDS = 16384, PAGE_BYTES = 256 };

typedef struct {
    const RecompCodeRegion *region;
    uint32_t index;
    uint8_t bytes[PAGE_BYTES];
} VerifiedPage;
typedef struct {
    uint32_t word;
    int next;
    const RecompCodeSwitch *jump_table;
    bool present, queued;
} PlannedInstruction;

typedef struct {
    PlannedInstruction *instructions;
    unsigned count, capacity;
    int pending;
    VerifiedPage page;
    const char *declined;
    uint32_t declined_pc;
} Plan;

typedef struct Observation {
    struct Observation *next;
    uint32_t pc, section, target, target_section;
} Observation;

enum DiscoveryKind { DIRECT_CALL = 1, INDIRECT_CALL = 2, JUMP_TARGET = 4, LIVE_FUNCTION = 8 };

typedef struct DiscoveredTarget {
    struct DiscoveredTarget *next;
    uint32_t section, address;
    unsigned kinds;
} DiscoveredTarget;

// Guest threads can yield inside a callback and be destroyed without returning.
// Keep any heap storage they hold reachable for the lifetime of the runtime.
typedef struct ActivePlan {
    struct ActivePlan *next;
    PlannedInstruction *instructions;
} ActivePlan;

typedef struct {
    Observation *observations;
    DiscoveredTarget *targets;
    ActivePlan *active;
    bool incomplete;
} InterpreterState;

static InterpreterState *interpreter_state(Runtime *rt) {
    if (!rt->cpu_observations)
        rt->cpu_observations = calloc(1, sizeof(InterpreterState));
    return rt->cpu_observations;
}

static void note_target(InterpreterState *state, uint32_t section, uint32_t address, enum DiscoveryKind kind) {
    for (DiscoveredTarget *t = state->targets; t; t = t->next) {
        if (t->section == section && t->address == address) {
            t->kinds |= kind;
            return;
        }
    }
    DiscoveredTarget *t = malloc(sizeof(*t));
    if (t) {
        *t = (DiscoveredTarget){state->targets, section, address, kind};
        state->targets = t;
    } else
        state->incomplete = true;
}

void recomp_note_live_discovery(Runtime *rt, uint32_t section, uint32_t address, bool jump_target) {
    if (!jump_target && recomp_has_section_function(rt, address, section))
        return;
    InterpreterState *state = interpreter_state(rt);
    if (state)
        note_target(state, section, address, jump_target ? JUMP_TARGET : LIVE_FUNCTION);
}

static bool resume_instruction(uint32_t w) {
    return w == 0x03e00008;
}

// COP1 operations use the same rounding, exception and register-pair helpers as generated code.
static bool fp_arithmetic(MipsContext *ctx, uint32_t w, bool fr1) {
    unsigned ft = w >> 16 & 31, fs = w >> 11 & 31, fd = w >> 6 & 31, fn = w & 63;
    unsigned format = w >> 21 & 31;
    enum MipsFpFormat fmt;
    switch (format) {
        case 16:
            fmt = MIPS_FP_S;
            break;
        case 17:
            fmt = MIPS_FP_D;
            break;
        case 20:
            fmt = MIPS_FP_W;
            break;
        case 21:
            fmt = MIPS_FP_L;
            break;
        default:
            return false;
    }
    if ((fn >= 8 && fn <= 15) || fn == 32 || fn == 33 || fn == 36 || fn == 37) {
        bool fixed_rounding = fn <= 15;
        enum MipsFpFormat to = fixed_rounding ? (fn < 12 ? MIPS_FP_L : MIPS_FP_W)
                               : fn == 32     ? MIPS_FP_S
                               : fn == 33     ? MIPS_FP_D
                               : fn == 36     ? MIPS_FP_W
                                              : MIPS_FP_L;
        if (ft || to == fmt || ((to == MIPS_FP_W || to == MIPS_FP_L) && (fmt == MIPS_FP_W || fmt == MIPS_FP_L)) ||
            (!fr1 && (((fmt == MIPS_FP_D || fmt == MIPS_FP_L) && (fs & 1)) ||
                      ((to == MIPS_FP_D || to == MIPS_FP_L) && (fd & 1)))))
            return false;
        if (ctx)
            mips_fp_convert(ctx, fd, fs, fmt, to, fixed_rounding ? (int)(fn & 3) : -1);
        return true;
    }
    if (fmt != MIPS_FP_S && fmt != MIPS_FP_D)
        return false;
    bool binary = fn <= 3, compare = fn >= 48;
    if (fmt == MIPS_FP_D && !fr1 && ((fs & 1) || ((binary || compare) && (ft & 1))))
        return false;
    if (!binary && !compare && ft)
        return false;
    if (compare) {
        if (fd)
            return false;
        if (ctx)
            mips_fp_compare(ctx, fs, ft, fmt, fn & 15);
    } else if (binary || fn <= 7) {
        if (fmt == MIPS_FP_D && !fr1 && (fd & 1))
            return false;
        if (ctx && binary)
            mips_fp_binary(ctx, fd, fs, ft, fmt, (enum MipsFpOperation)fn);
        else if (ctx) {
            const enum MipsFpOperation unary[] = {MIPS_FP_SQRT, MIPS_FP_ABS, MIPS_FP_MOV, MIPS_FP_NEG};
            mips_fp_unary(ctx, fd, fs, fmt, unary[fn - 4]);
        }
    } else
        return false;
    return true;
}

static bool ordinary(Runtime *rt, MipsContext *ctx, uint32_t w, bool fr1) {
    unsigned op = w >> 26, rs = w >> 21 & 31, reg = w >> 16 & 31, rd = w >> 11 & 31;
    unsigned shift = w >> 6 & 31;
    uint64_t a = ctx && rs ? ctx->gpr[rs] : 0, b = ctx && reg ? ctx->gpr[reg] : 0;
    uint64_t result;
    uint32_t addr = (uint32_t)a + (int16_t)w;
    unsigned dest = reg;
    switch (op) {
        case 0:
            dest = rd;
            if ((w & 63) <= 3 ? rs != 0 : shift != 0)
                return false;
            switch (w & 63) {
                case 0:
                    result = mips_s32((uint32_t)b << shift);
                    break;
                case 2:
                    result = mips_s32((uint32_t)b >> shift);
                    break;
                case 3:
                    result = mips_sra32(b, shift);
                    break;
                case 4:
                    result = mips_s32((uint32_t)b << (a & 31));
                    break;
                case 6:
                    result = mips_s32((uint32_t)b >> (a & 31));
                    break;
                case 7:
                    result = mips_sra32(b, a & 31);
                    break;
                case 33:
                    result = mips_s32(a + b);
                    break;
                case 35:
                    result = mips_s32(a - b);
                    break;
                case 36:
                    result = a & b;
                    break;
                case 37:
                    result = a | b;
                    break;
                case 38:
                    result = a ^ b;
                    break;
                case 39:
                    result = ~(a | b);
                    break;
                case 42:
                    result = mips_signed_less(a, b);
                    break;
                case 43:
                    result = a < b;
                    break;
                case 45:
                    result = a + b;
                    break;
                case 47:
                    result = a - b;
                    break;
                default:
                    return false;
            }
            break;
        case 9:
            result = mips_s32(addr);
            break;
        case 10:
            result = mips_signed_less(a, mips_s16(w));
            break;
        case 11:
            result = a < mips_s16(w);
            break;
        case 12:
            result = a & (uint16_t)w;
            break;
        case 13:
            result = a | (uint16_t)w;
            break;
        case 14:
            result = a ^ (uint16_t)w;
            break;
        case 15:
            if (rs)
                return false;
            result = mips_s32(w << 16);
            break;
        case 25:
            result = a + mips_s16(w);
            break;
        case 32:
        case 33:
        case 35:
        case 36:
        case 37:
        case 39:
        case 55:
            if (!ctx)
                return true;
            if (op == 55)
                result = recomp_load64(rt, addr);
            else if (op == 35 || op == 39) {
                uint32_t v = recomp_load32(rt, addr);
                result = op == 35 ? mips_s32(v) : v;
            } else if (op == 33 || op == 37) {
                uint16_t v = recomp_load16(rt, addr);
                result = op == 33 ? mips_s16(v) : v;
            } else {
                uint8_t v = recomp_load8(rt, addr);
                result = op == 32 ? mips_s8(v) : v;
            }
            break;
        case 40:
            if (ctx)
                recomp_store8(rt, addr, b);
            return true;
        case 41:
            if (ctx)
                recomp_store16(rt, addr, b);
            return true;
        case 43:
            if (ctx)
                recomp_store32(rt, addr, b);
            return true;
        case 63:
            if (ctx)
                recomp_store64(rt, addr, b);
            return true;
        case 49:
            if (ctx)
                ctx->fpr[reg] = recomp_load32(rt, addr);
            return true;
        case 57:
            if (ctx)
                recomp_store32(rt, addr, ctx->fpr[reg]);
            return true;
        case 53:
        case 61:
            if (!fr1 && (reg & 1))
                return false;
            if (ctx && op == 53)
                mips_set_fpr_pair(ctx, reg, recomp_load64(rt, addr));
            else if (ctx)
                recomp_store64(rt, addr, mips_fpr_pair(ctx, reg));
            return true;
        case 17:
            if (rs >= 16)
                return fp_arithmetic(ctx, w, fr1);
            if (w & 2047)
                return false;
            if (rs == 4) {
                if (ctx)
                    ctx->fpr[rd] = (uint32_t)b;
                return true;
            }
            if (rs != 0)
                return false;
            result = ctx ? mips_s32(ctx->fpr[rd]) : 0;
            break;
        default:
            return false;
    }
    if (ctx && dest)
        ctx->gpr[dest] = result;
    return true;
}

static bool in_region(const RecompCodeRegion *region, uint32_t pc) {
    return pc >= region->vram && (uint64_t)(pc - region->vram) + 4 <= region->size;
}

static bool excluded(const RecompCodeRegion *region, uint32_t pc) {
    size_t lo = 0, hi = region->excluded_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (pc < region->excluded[mid].first)
            hi = mid;
        else if (pc > region->excluded[mid].last)
            lo = mid + 1;
        else
            return true;
    }
    return false;
}

static bool read_word(Runtime *rt, uint32_t sid, uint32_t pc, uint32_t *word) {
    if (!rt->section_loaded[sid] || !in_region(&rt->code_regions[sid], pc) ||
        recomp_relocated_address(rt, sid, pc) != pc)
        return false;
    bool mapped;
    bool readable = recomp_probe_instruction(rt, pc, word, &mapped);
    const RecompCodeRegion *section = &rt->code_regions[sid];
    if (mapped || section->prepared_page_hashes)
        return readable;
    // With no installed TLB mapping, a profile can still identify the same
    // cartridge text in a resident direct-mapped segment. Never override an
    // active mapping or choose between different physical copies.
    uint64_t offset = (uint64_t)section->rom_offset + (pc - section->vram);
    uint32_t physical = UINT32_MAX;
    for (size_t i = 0; i < rt->section_count; i++) {
        const RecompCodeRegion *alias = &rt->code_regions[i];
        if (!rt->section_loaded[i] || alias->prepared_page_hashes || offset < alias->rom_offset ||
            offset + 4 > (uint64_t)alias->rom_offset + alias->size)
            continue;
        uint32_t address = alias->vram + (uint32_t)(offset - alias->rom_offset);
        if (!recomp_direct_address(address) || recomp_relocated_address(rt, (uint32_t)i, address) != address)
            continue;
        uint32_t candidate = address & UINT32_C(0x1fffffff);
        if (physical != UINT32_MAX && physical != candidate)
            return false;
        physical = candidate;
    }
    if (physical == UINT32_MAX)
        return readable;
    return recomp_probe_instruction(rt, physical | UINT32_C(0x80000000), word, &mapped);
}

static bool cartridge_word(Runtime *rt, const RecompCodeRegion *region, uint32_t pc, uint32_t *word) {
    if (!in_region(region, pc))
        return false;
    uint64_t offset = (uint64_t)region->rom_offset + (pc - region->vram);
    if (offset + 4 > rt->rom_size)
        return false;
    const uint8_t *bytes = rt->rom + offset;
    *word = (uint32_t)bytes[0] << 24 | (uint32_t)bytes[1] << 16 | (uint32_t)bytes[2] << 8 | bytes[3];
    return true;
}

static bool fetch(Runtime *rt, VerifiedPage *page, uint32_t sid, uint32_t pc, uint32_t *word) {
    if (!read_word(rt, sid, pc, word))
        return false;
    const RecompCodeRegion *r = &rt->code_regions[sid];
    uint32_t off = pc - r->vram;
    if (!r->prepared_page_hashes) {
        uint32_t expected;
        return cartridge_word(rt, r, pc, &expected) && *word == expected;
    }
    uint32_t index = off / PAGE_BYTES, start = index * PAGE_BYTES;
    if (page->region != r || page->index != index) {
        size_t n = r->size - start;
        if (n > PAGE_BYTES)
            n = PAGE_BYTES;
        for (size_t j = 0; j < n; j += 4) {
            uint32_t w;
            if (!read_word(rt, sid, r->vram + start + (uint32_t)j, &w))
                return false;
            for (unsigned k = 0; k < 4; k++)
                page->bytes[j + k] = (uint8_t)(w >> (24 - k * 8));
        }
        uint8_t digest[32];
        if (!recomp_sha256(page->bytes, n, digest) || memcmp(digest, r->prepared_page_hashes[index], 32))
            return false;
        page->region = r;
        page->index = index;
    }
    const uint8_t *p = page->bytes + off % PAGE_BYTES;
    return *word == ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]);
}

static bool decline_plan(Plan *p, uint32_t pc, const char *reason) {
    p->declined_pc = pc;
    p->declined = reason;
    return false;
}

static bool plan_word(Runtime *rt, Plan *p, uint32_t start, uint32_t sid, unsigned i) {
    if (i >= MAX_WORDS || (uint64_t)start + i * 4 > UINT32_MAX)
        return decline_plan(p, start + i * 4, "outside the interpreter window");
    uint32_t pc = start + i * 4;
    if (i >= p->capacity) {
        unsigned capacity = p->capacity ? p->capacity : 32;
        while (capacity <= i)
            capacity *= 2;
        PlannedInstruction *items = realloc(p->instructions, capacity * sizeof(*items));
        if (!items)
            return decline_plan(p, pc, "unable to allocate instruction plan");
        memset(items + p->capacity, 0, (capacity - p->capacity) * sizeof(*items));
        p->instructions = items;
        p->capacity = capacity;
    }
    PlannedInstruction *item = &p->instructions[i];
    if (!item->present) {
        if (excluded(&rt->code_regions[sid], pc))
            return decline_plan(p, pc, "profile boundary");
        if (recomp_has_section_function(rt, pc, sid))
            return decline_plan(p, pc, "reached another function in the loaded section");
        if (!fetch(rt, &p->page, sid, pc, &item->word))
            return decline_plan(p, pc, "instructions differ from the mapped ROM image or are unreadable");
        item->present = true;
        if (i >= p->count)
            p->count = i + 1;
    }
    return true;
}

static uint32_t jump_address(uint32_t pc, uint32_t word) {
    return ((pc + 4) & 0xf0000000u) | ((word & 0x03ffffffu) << 2);
}

static bool indirect_call(uint32_t word) {
    return (word & 0xfc1fffff) == 0xf809; // JALR with the standard link register.
}

static bool conditional_branch(uint32_t w) {
    unsigned op = w >> 26, rt = w >> 16 & 31;
    if (op == 1)
        return rt < 4; // BLTZ, BGEZ and their likely variants.
    if (op == 17)
        return (w >> 21 & 31) == 8 && rt < 4;
    return op == 4 || op == 5 || op == 20 || op == 21 || ((op == 6 || op == 7 || op == 22 || op == 23) && !rt);
}

static bool branch_taken(const MipsContext *ctx, uint32_t w) {
    unsigned op = w >> 26, rs = w >> 21 & 31, rt = w >> 16 & 31;
    uint64_t a = rs ? ctx->gpr[rs] : 0, b = rt ? ctx->gpr[rt] : 0;
    if (op == 1)
        return mips_signed_less(a, 0) != (bool)(rt & 1);
    if (op == 17)
        return !!(ctx->fcr31 & 0x800000) == !!(rt & 1);
    switch (op & 15) {
        case 4:
            return a == b;
        case 5:
            return a != b;
        case 6:
            return !a || mips_signed_less(a, 0);
        default:
            return a && !mips_signed_less(a, 0);
    }
}

// Queue by index so growing the worklist never invalidates a successor.
// A delay slot may also be entered independently. Validate its word both ways.
static bool queue_entry(Runtime *rt, Plan *p, uint32_t start, uint32_t sid, unsigned i) {
    if (!plan_word(rt, p, start, sid, i))
        return false;
    PlannedInstruction *item = &p->instructions[i];
    if (!item->queued) {
        item->queued = true;
        item->next = p->pending;
        p->pending = (int)i;
    }
    return true;
}

static const RecompCodeSwitch *find_switch(const RecompCodeRegion *region, uint32_t pc, uint32_t word) {
    size_t lo = 0, hi = region->switch_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const RecompCodeSwitch *sw = &region->switches[mid];
        if (sw->pc == pc)
            return sw->word == word ? sw : NULL;
        if (sw->pc < pc)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

static bool preflight(Runtime *rt, Plan *p, uint32_t start, uint32_t sid, bool fr1) {
    if (!queue_entry(rt, p, start, sid, 0))
        return false;
    while (p->pending >= 0) {
        unsigned i = (unsigned)p->pending;
        p->pending = p->instructions[i].next;
        uint32_t w = p->instructions[i].word, op = w >> 26;
        if (ordinary(NULL, NULL, w, fr1)) {
            if (!queue_entry(rt, p, start, sid, i + 1))
                return false;
            continue;
        }
        const RecompCodeSwitch *sw = NULL;
        if ((w & 0xfc1fffff) == 8 && !resume_instruction(w)) {
            sw = find_switch(&rt->code_regions[sid], start + i * 4, w);
            if (!sw)
                return decline_plan(p, start + i * 4, "computed jump has no verified targets");
            p->instructions[i].jump_table = sw;
        }
        if (!(sw || resume_instruction(w) || op == 3 || indirect_call(w) || op == 2 || conditional_branch(w)))
            return decline_plan(p, start + i * 4, "unsupported instruction");
        if (!plan_word(rt, p, start, sid, i + 1))
            return false;
        if (!ordinary(NULL, NULL, p->instructions[i + 1].word, fr1))
            return decline_plan(p, start + (i + 1) * 4, "unsupported delay slot");
        if (resume_instruction(w))
            continue;
        if (sw) {
            for (size_t n = 0; n < sw->count; n++) {
                uint32_t dest = sw->targets[n];
                if (dest < start || (dest & 3))
                    return decline_plan(p, dest, "switch target outside the local function");
                if (!queue_entry(rt, p, start, sid, (dest - start) / 4))
                    return false;
            }
            continue;
        }
        if (op == 2) {
            uint32_t to = jump_address(start + i * 4, w);
            if (to < start)
                return decline_plan(p, to, "jump below the entry");
            if (!queue_entry(rt, p, start, sid, (to - start) / 4))
                return false;
            continue;
        }
        if (conditional_branch(w)) {
            bool same = ((w >> 21) & 31) == ((w >> 16) & 31);
            bool never = (op == 5 || op == 21) && same;
            if (!never && !queue_entry(rt, p, start, sid, i + 1 + (int16_t)w))
                return false;
            if ((op == 4 || op == 20) && same)
                continue;
        }
        if (!queue_entry(rt, p, start, sid, i + 2))
            return false;
    }
    return true;
}

static void verify_instruction(Runtime *rt, MipsContext *ctx, uint32_t sid, uint32_t pc, uint32_t expected) {
    uint32_t w;
    if (!read_word(rt, sid, pc, &w) || w != expected)
        recomp_fail("interpreted code changed during execution", pc);
    (void)ctx;
    RECOMP_TRACE(ctx, pc);
}

static void execute(Runtime *rt, MipsContext *ctx, const Plan *p, uint32_t start, uint32_t sid, uint32_t ra) {
    for (unsigned i = 0;;) {
        uint32_t pc = start + i * 4, w = p->instructions[i].word, op = w >> 26;
        verify_instruction(rt, ctx, sid, pc, w);
        if (ordinary(rt, ctx, w, ctx->fpu_fr1)) {
            i++;
            continue;
        }
        uint32_t destination = (uint32_t)ctx->gpr[31];
        bool taken = true;
        bool call = op == 3 || indirect_call(w);
        const RecompCodeSwitch *sw = p->instructions[i].jump_table;
        if (sw) {
            destination = (uint32_t)ctx->gpr[w >> 21 & 31];
            size_t n = 0;
            while (n < sw->count && sw->targets[n] != destination)
                n++;
            if (n == sw->count)
                recomp_fail("interpreted switch destination outside verified table", destination);
        } else if (call) {
            destination = op == 3 ? jump_address(pc, w) : (uint32_t)ctx->gpr[w >> 21 & 31];
            ctx->gpr[31] = mips_s32(pc + 8);
        } else if (conditional_branch(w))
            taken = branch_taken(ctx, w);
        bool likely = (op == 1 || op == 17) ? !!(w & (2u << 16)) : op >= 20 && op <= 23;
        if (taken || !likely) {
            verify_instruction(rt, ctx, sid, pc + 4, p->instructions[i + 1].word);
            ordinary(rt, ctx, p->instructions[i + 1].word, ctx->fpu_fr1);
        }
        if (resume_instruction(w)) {
            if (destination != ra)
                recomp_fail("unexpected interpreted return", destination);
            return;
        }
        if (call) {
            recomp_call_at(rt, ctx, destination, pc, sid);
            i += 2;
        } else if (op == 2 || sw) {
            uint32_t to = sw ? destination : jump_address(pc, w);
            if (to <= pc)
                recomp_checkpoint(rt);
            i = (to - start) / 4;
        } else if (taken) {
            if ((int16_t)w < 0)
                recomp_checkpoint(rt);
            i = (unsigned)((int)i + 1 + (int16_t)w);
        } else
            i += 2;
    }
}

static void hex_string(char *text, const uint8_t *bytes, size_t n) {
    for (size_t i = 0; i < n; i++)
        snprintf(text + 2 * i, 3, "%02x", bytes[i]);
}

static void record(Runtime *rt, uint32_t site, uint32_t section, uint32_t word, uint32_t target, uint32_t sid,
                   Plan *plan, bool jump) {
    InterpreterState *state = rt->cpu_observations;
    for (Observation *o = state->observations; o; o = o->next)
        if (o->pc == site && o->section == section && o->target == target && o->target_section == sid)
            return;
    note_target(state, sid, target, jump ? JUMP_TARGET : (indirect_call(word) ? INDIRECT_CALL : DIRECT_CALL));
    unsigned count = plan->count;
    const RecompCodeRegion *r = &rt->code_regions[sid];
    size_t offset = (size_t)r->rom_offset + (target - r->vram);
    uint8_t hash[32];
    uint8_t *bytes = NULL;
    const uint8_t *body;
    if (r->prepared_page_hashes) {
        bytes = malloc(count * 4);
        if (!bytes)
            return;
        // Include gaps between branch arms in the fingerprint used by static discovery.
        for (unsigned i = 0; i < count; i++) {
            uint32_t w;
            if (!fetch(rt, &plan->page, sid, target + 4 * i, &w)) {
                free(bytes);
                return;
            }
            for (unsigned j = 0; j < 4; j++)
                bytes[4 * i + j] = (uint8_t)(w >> (24 - 8 * j));
        }
        body = bytes;
    } else
        body = rt->rom + offset;
    bool hashed = recomp_sha256(body, count * 4, hash);
    free(bytes);
    if (!hashed)
        return;
    char sha[65], rom[41], path[4096], temp[4096];
    hex_string(sha, hash, 32);
    hex_string(rom, rt->rom_sha1, 20);
    if (snprintf(path, sizeof(path), "%s/%s-%s-%u-%08x-%u-%08x.json", rt->observation_dir, rom, rt->code_layout,
                 section, site, sid, target) >= (int)sizeof(path) ||
        snprintf(temp, sizeof(temp), "%s.%ld.tmp", path, (long)getpid()) >= (int)sizeof(temp))
        return;
    FILE *f = fopen(temp, "w");
    if (!f && errno == ENOENT) {
        // Create the output path only after a miss has actually returned.
        recomp_create_directories(rt->observation_dir);
        f = fopen(temp, "w");
    }
    if (!f) {
        fprintf(stderr, "CPU fallback: cannot write %s\n", temp);
        return;
    }
    int wrote =
        fprintf(f,
                "{\"rom\":\"%s\",\"layout\":\"%s\",\"section\":%u,\"pc\":%u,\"word\":%u,"
                "\"target_section\":%u,\"target\":%u,\"size\":%u,\"sha256\":\"%s\",\"jump\":%s}\n",
                rom, rt->code_layout, section, site, word, sid, target, count * 4, sha, jump ? "true" : "false");
    if (fclose(f) || wrote < 0 || rename(temp, path)) {
        remove(temp);
        return;
    }
    Observation *o = malloc(sizeof(*o));
    if (o) {
        *o = (Observation){state->observations, site, section, target, sid};
        state->observations = o;
    }
    fprintf(stderr, "CPU fallback: interpreted 0x%08x from 0x%08x; saved %s\n", target, site, path);
}

static bool decline_call(uint32_t target, uint32_t site, uint32_t pc, const char *reason) {
    fprintf(stderr, "CPU fallback: handing 0x%08x from 0x%08x to normal dispatch: %s at 0x%08x\n", target, site, reason,
            pc);
    return false;
}

uint32_t recomp_loaded_code_section(const Runtime *rt, uint32_t address) {
    uint32_t section = UINT32_MAX;
    if (rt->code_regions) {
        for (size_t i = 0; i < rt->section_count; i++) {
            if (!rt->section_loaded[i] || !in_region(&rt->code_regions[i], address))
                continue;
            if (section != UINT32_MAX)
                return UINT32_MAX;
            section = (uint32_t)i;
        }
    }
    return section;
}

bool recomp_interpret_call(Runtime *rt, MipsContext *ctx, uint32_t target, uint32_t site, uint32_t section, bool jump,
                           uint32_t ra) {
    if (!rt->code_regions || !rt->observation_dir || !rt->code_layout || !rt->rom)
        return false;
    if (!site || (target | site) & 3 || section >= rt->section_count)
        return decline_call(target, site, site, "missing or invalid caller location");
    uint32_t sid = recomp_loaded_code_section(rt, target), word;
    Plan plan = {.pending = -1};
    uint32_t delay_slot;
    if (sid == UINT32_MAX)
        return decline_call(target, site, target, "no unique loaded section");
    if (!fetch(rt, &plan.page, section, site, &word) || site > UINT32_MAX - 4 ||
        !fetch(rt, &plan.page, section, site + 4, &delay_slot))
        return decline_call(target, site, site,
                            "caller or delay slot differs from the mapped ROM image or is unreadable");
    if (jump) {
        if (excluded(&rt->code_regions[section], site) || (word & 0xfc1fffff) != 8 || (word >> 21 & 31) == 31)
            return decline_call(target, site, site, "unsupported jump site");
    } else if (ra != site + 8 || ((word >> 26) != 3 ? !indirect_call(word) : jump_address(site, word) != target))
        return decline_call(target, site, site, "call instruction or return address mismatch");
    if (rt->prepare_call)
        rt->prepare_call(rt, target);
    if (!preflight(rt, &plan, target, sid, ctx->fpu_fr1)) {
        free(plan.instructions);
        return decline_call(target, site, plan.declined_pc, plan.declined);
    }
    InterpreterState *state = interpreter_state(rt);
    ActivePlan *active = state ? malloc(sizeof(*active)) : NULL;
    if (!active) {
        free(plan.instructions);
        return decline_call(target, site, target, "unable to retain instruction plan");
    }
    *active = (ActivePlan){state->active, plan.instructions};
    state->active = active;
    execute(rt, ctx, &plan, target, sid, ra);
    record(rt, site, section, word, target, sid, &plan, jump);
    ActivePlan **link = &state->active;
    while (*link != active)
        link = &(*link)->next;
    *link = active->next;
    free(active);
    free(plan.instructions);
    return true;
}

void recomp_interpreter_destroy(Runtime *rt) {
    InterpreterState *state = rt->cpu_observations;
    size_t functions = 0, direct = 0, indirect = 0, jumps = 0, live = 0, saved = 0;
    DiscoveredTarget *t = state ? state->targets : NULL;
    while (t) {
        functions += !!(t->kinds & (DIRECT_CALL | INDIRECT_CALL | LIVE_FUNCTION));
        direct += !!(t->kinds & DIRECT_CALL);
        indirect += !!(t->kinds & INDIRECT_CALL);
        live += !!(t->kinds & LIVE_FUNCTION);
        jumps += !!(t->kinds & JUMP_TARGET);
        DiscoveredTarget *next = t->next;
        free(t);
        t = next;
    }
    if (rt->live_patch_game) {
        for (Observation *o = state ? state->observations : NULL; o; o = o->next)
            saved++;
        fprintf(stderr,
                "CPU discovery for %s%s\n"
                "  Functions in this build: %zu\n"
                "  Distinct functions discovered this run: %zu\n"
                "    Interpreted via direct call (JAL): %zu\n"
                "    Interpreted via indirect call (JALR): %zu\n"
                "    Provided by live repair: %zu\n"
                "  Computed jump destinations (JR): %zu\n"
                "  Interpreter observations saved this run: %zu\n",
                rt->live_patch_game, state && state->incomplete ? " (partial count)" : "", rt->function_count,
                functions, direct, indirect, live, jumps, saved);
    }
    if (!state)
        return;
    while (state->active) {
        ActivePlan *active = state->active;
        state->active = active->next;
        free(active->instructions);
        free(active);
    }
    Observation *o = state->observations;
    while (o) {
        Observation *next = o->next;
        free(o);
        o = next;
    }
    free(state);
    rt->cpu_observations = NULL;
}
