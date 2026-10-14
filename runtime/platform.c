#include "runtime.h"
#include <stdlib.h>

typedef struct AiBuffer {
    uint32_t address, length, dac_rate, start_fraction, end_fraction;
    uint64_t start, end;
} AiBuffer;

struct RecompPlatform {
    RecompPlatformConfig config;
    RecompViState vi;
    RecompAiState ai;
    AiBuffer ai_buffers[2];
    RecompAiOutput ai_output;
    uint64_t ai_pending_events;
    bool initialized, pi_created;
    uint32_t vi_queue, vi_message, pi_queue, mode_ctrl;
    uint64_t next_vi, retrace_phase;
    uint32_t *code_received;
};

static RecompPlatform *platform(Runtime *rt) {
    if (!rt->platform) recomp_fail("native platform not initialized", 0);
    return rt->platform;
}
static uint32_t arg(MipsContext *ctx, unsigned index) { return (uint32_t)ctx->gpr[4 + index]; }
static void result(MipsContext *ctx, int32_t value) { mips_set(ctx, 2, mips_s32((uint32_t)value)); }

static void start_ai(Runtime *rt, AiBuffer *buffer, uint64_t start, uint32_t fraction) {
    RecompPlatform *p = platform(rt);
    uint64_t units = (uint64_t)(buffer->length / 4) * recomp_clock_frequency(rt) * buffer->dac_rate + fraction;
    uint64_t ticks = units / p->ai.clock;
    if (ticks >= UINT64_MAX - start) recomp_fail("AI DMA schedule exhausted", buffer->address);
    buffer->start = start; buffer->start_fraction = fraction;
    buffer->end = start + ticks; buffer->end_fraction = (uint32_t)(units % p->ai.clock);
    if (p->ai_output.start && !p->ai_output.start(rt, p->ai_output.opaque, buffer->address,
                                               buffer->length, p->ai.clock, buffer->dac_rate))
        recomp_fail("native AI output failed", buffer->address);
}

static void poll_ai(Runtime *rt) {
    RecompPlatform *p = platform(rt);
    uint64_t now = recomp_count64(rt);
    while (p->ai.queued) {
        AiBuffer *b = &p->ai_buffers[0];
        if (now < b->end || (now == b->end && b->end_fraction)) break;
        uint64_t next_start = b->end; uint32_t fraction = b->end_fraction;
        p->ai.completed++; p->ai_pending_events++; p->ai.queued--;
        if (p->ai.queued) {
            p->ai_buffers[0] = p->ai_buffers[1];
            start_ai(rt, &p->ai_buffers[0], next_start, fraction);
        }
    }
    p->ai.address = p->ai.length = 0;
    if (p->ai.queued) {
        const AiBuffer *b = &p->ai_buffers[0];
        uint64_t units = (now - b->start) * p->ai.clock;
        uint64_t samples = units < b->start_fraction ? 0
                         : (units - b->start_fraction) / (recomp_clock_frequency(rt) * b->dac_rate);
        p->ai.address = b->address + (uint32_t)samples * 4;
        p->ai.length = (b->length - (uint32_t)samples * 4) & ~UINT32_C(7);
    }
    /* Owner-thread transport. Retry a full pending FIFO; guest queues retain
     * their ordinary nonblocking drop behavior after scheduler delivery. */
    while (p->ai_pending_events && recomp_event_post(rt, 6)) {
        p->ai_pending_events--; p->ai.events++;
    }
}

static bool rom_range(Runtime *rt, uint32_t *offset, uint32_t bytes) {
    if (*offset >= UINT32_C(0xb0000000) && *offset < UINT32_C(0xc0000000)) *offset -= UINT32_C(0xb0000000);
    else if (*offset >= UINT32_C(0x10000000) && *offset < UINT32_C(0x20000000)) *offset -= UINT32_C(0x10000000);
    return rt->rom && *offset <= rt->rom_size && bytes <= rt->rom_size - *offset;
}

static void code_loaded(Runtime *rt, uint32_t address, uint32_t offset, uint32_t size) {
    RecompPlatform *p = platform(rt);
    uint32_t physical = recomp_ram_address(rt, address, size);
    for (size_t i = 0; i < p->config.code_load_count; i++) {
        const RecompBootLoad *s = &p->config.code_loads[i];
        uint32_t base = recomp_ram_address(rt, s->vram, s->size);
        uint64_t end = (uint64_t)physical + size, section_end = (uint64_t)base + s->size;
        if (physical >= section_end || base >= end) continue;
        uint32_t first = physical > base ? physical : base;
        uint32_t last = end < section_end ? (uint32_t)end : (uint32_t)section_end;
        rt->section_loaded[s->section] = false;
        if ((uint64_t)offset + first - physical != (uint64_t)s->rom_offset + first - base) {
            p->code_received[i] = 0; continue;
        }
        if (first == base) p->code_received[i] = 0;
        if (first - base == p->code_received[i]) p->code_received[i] += last - first;
        else p->code_received[i] = 0;
        if (p->code_received[i] == s->size) rt->section_loaded[s->section] = true;
    }
}

static void poll_platform(Runtime *rt) {
    RecompPlatform *p = platform(rt);
    uint64_t now = recomp_count64(rt);
    if (p->initialized) poll_ai(rt);
    if (p->pi_created) {
        /* The native PI service replaces the privileged manager thread. */
        for (unsigned i = 0; i < 256; i++) {
            /* Peek before dequeue: the message is a guest OSIoMesg pointer. */
            uint32_t mb;
            if (recomp_queue_peek(rt, p->pi_queue, &mb)) break;
            if (recomp_queue_recv(rt, p->pi_queue, 0, RECOMP_MESG_NOBLOCK)) break;
            recomp_ram_address(rt, mb, 20);
            uint32_t address = (uint32_t)recomp_load(rt, mb + 8, 4);
            uint32_t offset = (uint32_t)recomp_load(rt, mb + 12, 4);
            uint32_t size = (uint32_t)recomp_load(rt, mb + 16, 4);
            uint32_t queue = (uint32_t)recomp_load(rt, mb + 4, 4);
            if (recomp_load(rt, mb, 2) != 11 || !size || !rom_range(rt, &offset, size))
                recomp_fail("unsupported/corrupt queued PI transfer", mb);
            recomp_copy_to_ram(rt, address, rt->rom + offset, size);
            code_loaded(rt, address, offset, size);
            if (queue) (void)recomp_queue_send(rt, queue, mb, RECOMP_MESG_NOBLOCK, false);
        }
    }
    if (!p->vi.manager_created || now < p->next_vi) return;
    uint64_t due = (now - p->next_vi) / p->config.vi_period_ticks + 1;
    if (due > (UINT64_MAX - p->next_vi) / p->config.vi_period_ticks || due > UINT64_MAX - p->vi.retraces)
        recomp_fail("VI schedule exhausted", 0);
    p->next_vi += due * p->config.vi_period_ticks;
    p->vi.retraces += due;
    p->vi.current_framebuffer = p->vi.framebuffer;
    uint64_t count = p->vi.retrace_count ? p->vi.retrace_count : 1;
    uint64_t deliveries = due / count + (due % count + p->retrace_phase) / count;
    p->retrace_phase = (due % count + p->retrace_phase) % count;
    if (p->config.vi_intr_count) recomp_store(rt, p->config.vi_intr_count, (uint32_t)p->vi.retraces, 4);
    if (p->config.current_time) recomp_store(rt, p->config.current_time, recomp_get_time(rt), 8);
    if (p->config.base_counter) recomp_store(rt, p->config.base_counter, (uint32_t)now, 4);
    if (p->vi_queue) {
        /* Bounded work; excess overdue presentation notifications are dropped. */
        if (deliveries > 256) deliveries = 256;
        for (uint64_t i = 0; i < deliveries; i++)
            (void)recomp_queue_send(rt, p->vi_queue, p->vi_message, RECOMP_MESG_NOBLOCK, false);
    }
}

bool recomp_platform_init(Runtime *rt, const RecompPlatformConfig *config) {
    if (rt->platform || !rt->clock || !rt->scheduler || !rt->rom || !config || !config->vi_period_ticks
            || (config->code_load_count && !config->code_loads)) return false;
    uint32_t addresses[] = {config->clock_rate, config->final_rom, config->timer_list,
        config->current_time, config->base_counter, config->vi_intr_count, config->app_nmi_buffer, config->vi_clock};
    const unsigned sizes[] = {8,4,4,8,4,4,64,4};
    for (unsigned i = 0; i < sizeof(addresses)/sizeof(addresses[0]); i++)
        if (addresses[i]) {
            if (addresses[i] % (sizes[i] == 8 ? 8 : 4)) return false;
            recomp_ram_address(rt, addresses[i], sizes[i]);
        }
    for (size_t i = 0; i < config->code_load_count; i++) {
        const RecompBootLoad *s = &config->code_loads[i];
        uint32_t offset = s->rom_offset;
        if (s->section >= rt->section_count || !s->size || s->vram % 4 || s->size % 4
                || !rom_range(rt, &offset, s->size) || offset != s->rom_offset)
            return false;
        uint32_t physical = recomp_ram_address(rt, s->vram, s->size);
        for (size_t j = 0; j < i; j++) {
            const RecompBootLoad *old = &config->code_loads[j];
            uint32_t other = recomp_ram_address(rt, old->vram, old->size);
            if (old->section == s->section
                    || (physical < (uint64_t)other + old->size && other < (uint64_t)physical + s->size)) return false;
        }
    }
    RecompPlatform *p = calloc(1, sizeof(*p));
    if (!p) return false;
    p->config = *config;
    p->code_received = calloc(config->code_load_count ? config->code_load_count : 1, sizeof(uint32_t));
    if (!p->code_received) { free(p); return false; }
    p->vi.retrace_count = 1;
    rt->platform = p; rt->destroy_platform = recomp_platform_destroy; rt->poll_platform = poll_platform;
    return true;
}

void recomp_platform_destroy(Runtime *rt) {
    if (!rt->platform) return;
    free(rt->platform->code_received); free(rt->platform);
    rt->platform = NULL; rt->destroy_platform = NULL; rt->poll_platform = NULL;
    rt->platform_initialized = false;
}
RecompViState recomp_vi_state(Runtime *rt) { return platform(rt)->vi; }
RecompAiState recomp_ai_state(Runtime *rt) {
    if (platform(rt)->initialized) poll_ai(rt);
    return platform(rt)->ai;
}
uint64_t recomp_platform_next_deadline(Runtime *rt) {
    const RecompPlatform *p = platform(rt);
    uint64_t next = p->vi.manager_created ? p->next_vi : UINT64_MAX;
    if (p->ai.queued) {
        const AiBuffer *head = &p->ai_buffers[0];
        /* poll_ai completes fractional endpoints on the following tick. */
        uint64_t end = head->end + (head->end_fraction != 0);
        if (end < next) next = end;
    }
    return next;
}
bool recomp_ai_set_output(Runtime *rt, const RecompAiOutput *output) {
    if (!rt->platform) return false;
    /* Detaching is always permitted so a sink can be destroyed before Runtime.
     * Late attachment would miss PCM for an already active DMA. */
    if (output && platform(rt)->ai.queued) return false;
    platform(rt)->ai_output = output ? *output : (RecompAiOutput){0};
    return true;
}

void hook_os_ai_get_length(Runtime *rt, MipsContext *ctx) {
    RecompAiState state = recomp_ai_state(rt); mips_set(ctx, 2, state.length);
}
void hook_os_ai_get_status(Runtime *rt, MipsContext *ctx) {
    RecompAiState state = recomp_ai_state(rt);
    mips_set(ctx, 2, mips_s32((state.queued ? UINT32_C(0x40000000) : 0)
                 | (state.queued == 2 ? UINT32_C(0x80000000) : 0)));
}
void hook_os_ai_set_next_buffer(Runtime *rt, MipsContext *ctx) {
    RecompPlatform *p = platform(rt);
    if (!p->initialized) recomp_fail("AI DMA before OS initialization", ctx->pc);
    poll_ai(rt);
    if (p->ai.queued == 2) { p->ai.rejected++; result(ctx, -1); return; }
    uint32_t address = arg(ctx, 0), bytes = arg(ctx, 1);
    if (address % 8 || bytes % 8 || !bytes || bytes > UINT32_C(0x3fff8))
        recomp_fail("unsupported or unaligned AI buffer", ctx->pc);
    recomp_ram_address(rt, address, bytes);
    AiBuffer buffer = {.address=address,.length=bytes,.dac_rate=p->ai.dac_rate};
    if (!p->ai.queued) start_ai(rt, &buffer, recomp_count64(rt), 0);
    p->ai_buffers[p->ai.queued++] = buffer; p->ai.submissions++;
    poll_ai(rt); result(ctx, 0);
}

void hook_os_ai_set_frequency(Runtime *rt, MipsContext *ctx) {
    RecompPlatform *p = platform(rt);
    if (!p->initialized) recomp_fail("AI frequency before OS initialization", ctx->pc);
    poll_ai(rt);
    if (p->ai.queued) recomp_fail("AI frequency changes during DMA require explicit support", ctx->pc);
    uint32_t frequency = arg(ctx, 0), clock = p->ai.clock;
    if (!frequency) recomp_fail("unsupported zero AI frequency", ctx->pc);
    if (frequency > clock) { result(ctx, -1); return; }
    /* Preserve SDK binary32 rounding; a double/integer shortcut can choose a
     * different divisor. Only the temporary context's scratch FPRs change. */
    MipsContext scratch = *ctx;
    scratch.fpr[0] = clock;
    mips_fp_convert(&scratch, 2, 0, MIPS_FP_W, MIPS_FP_S, -1);
    scratch.fpr[0] = frequency;
    mips_fp_convert(&scratch, 4, 0, MIPS_FP_W, MIPS_FP_S, -1);
    mips_fp_binary(&scratch, 6, 2, 4, MIPS_FP_S, MIPS_FP_DIV);
    scratch.fpr[2] = UINT32_C(0x3f000000);
    mips_fp_binary(&scratch, 8, 6, 2, MIPS_FP_S, MIPS_FP_ADD);
    uint32_t saved_fcr31 = scratch.fcr31;
    mips_fp_convert(&scratch, 10, 8, MIPS_FP_S, MIPS_FP_W, 1);
    ctx->fcr31 = saved_fcr31; /* SDK restores FCSR saved before its truncation. */
    uint32_t dac = scratch.fpr[10];
    if (dac < 132) { result(ctx, -1); return; }
    uint32_t bit_rate = (dac / 66) & 255;
    if (bit_rate > 16) bit_rate = 16;
    /* Future AI DMA support must model the hardware's 14-bit DAC register for
     * unusually low rates. This startup subset rejects that unsupported case. */
    if (dac > 16384) recomp_fail("unsupported AI divisor above hardware range", ctx->pc);
    p->ai.dac_rate = dac; p->ai.bit_rate = bit_rate; p->ai.frequency = clock / dac;
    p->ai.enabled = true; result(ctx, (int32_t)p->ai.frequency);
}

void hook_os_initialize(Runtime *rt, MipsContext *ctx) {
    RecompPlatform *p = platform(rt);
    if (p->initialized) recomp_fail("repeated OS initialization", ctx->pc);
    p->initialized = true;
    rt->platform_initialized = true;
    uint32_t tv = (uint32_t)recomp_load(rt, UINT32_C(0x80000300), 4);
    p->ai.clock = tv == 0 ? UINT32_C(49656530) : tv == 2 ? UINT32_C(48628316) : UINT32_C(48681812);
    if (p->config.vi_clock) recomp_store(rt, p->config.vi_clock, p->ai.clock, 4);
    p->ai.dac_rate = 16384; p->ai.bit_rate = 16; p->ai.enabled = true;
    p->ai.frequency = p->ai.clock / p->ai.dac_rate;
    if (p->config.clock_rate) recomp_store(rt, p->config.clock_rate, recomp_clock_frequency(rt), 8);
    if (p->config.final_rom) recomp_store(rt, p->config.final_rom, 1, 4);
    if (p->config.app_nmi_buffer && !recomp_load(rt, UINT32_C(0x8000030c), 4))
        for (unsigned i = 0; i < 64; i++) recomp_store(rt, p->config.app_nmi_buffer + i, 0, 1);
    ctx->fcr31 = UINT32_C(0x01000800);
    if (p->config.timer_list) recomp_store(rt, p->config.timer_list, recomp_clock_timer_sentinel(rt), 4);
    if (rt->poll_clock) rt->poll_clock(rt);
    /* Exceptions/TLB/cache operations are replaced at the OS boundary. No
     * emulated exception vector or privileged CPU state is installed. */
}
void hook_os_create_vi_manager(Runtime *rt, MipsContext *ctx) {
    (void)ctx;
    RecompPlatform *p = platform(rt);
    if (p->vi.manager_created) return;
    uint64_t now = recomp_count64(rt);
    if (p->config.vi_period_ticks > UINT64_MAX - now) recomp_fail("VI schedule exhausted", 0);
    p->vi.manager_created = true; p->vi.black = true;
    p->next_vi = now + p->config.vi_period_ticks;
}
void hook_os_vi_set_mode(Runtime *rt, MipsContext *ctx) {
    RecompPlatform *p = platform(rt);
    recomp_ram_address(rt, arg(ctx, 0), 80);
    p->vi.mode = arg(ctx, 0);
    p->mode_ctrl = (uint32_t)recomp_load(rt, p->vi.mode + 4, 4);
    p->vi.special_features = p->mode_ctrl;
}
void hook_os_vi_black(Runtime *rt, MipsContext *ctx) { platform(rt)->vi.black = (uint8_t)arg(ctx, 0) != 0; }
void hook_os_vi_set_special_features(Runtime *rt, MipsContext *ctx) {
    RecompPlatform *p = platform(rt);
    uint32_t flags = arg(ctx, 0), bits[] = {8,4,16,UINT32_C(0x10000)};
    for (unsigned i = 0; i < 4; i++) {
        if (flags & (1u << (i*2))) p->vi.special_features |= bits[i];
        if (flags & (2u << (i*2))) p->vi.special_features &= ~bits[i];
    }
    if (flags & 0x40) p->vi.special_features &= ~UINT32_C(0x300);
    if (flags & 0x80) p->vi.special_features |= p->mode_ctrl & UINT32_C(0x300);
}
void hook_os_vi_set_event(Runtime *rt, MipsContext *ctx) {
    RecompPlatform *p = platform(rt);
    p->vi_queue = arg(ctx, 0); p->vi_message = arg(ctx, 1);
    p->vi.retrace_count = (uint16_t)arg(ctx, 2); p->retrace_phase = 0;
}
void hook_os_vi_swap_buffer(Runtime *rt, MipsContext *ctx) {
    recomp_ram_address(rt, arg(ctx, 0), 1);
    platform(rt)->vi.framebuffer = arg(ctx, 0);
}
void hook_os_vi_get_current_framebuffer(Runtime *rt, MipsContext *ctx) {
    result(ctx, (int32_t)platform(rt)->vi.current_framebuffer);
}
void hook_os_vi_get_next_framebuffer(Runtime *rt, MipsContext *ctx) {
    result(ctx, (int32_t)platform(rt)->vi.framebuffer);
}
void hook_os_create_pi_manager(Runtime *rt, MipsContext *ctx) {
    RecompPlatform *p = platform(rt);
    if (p->pi_created) return;
    recomp_queue_create(rt, arg(ctx, 1), arg(ctx, 2), (int32_t)mips_i32(ctx->gpr[7]));
    p->pi_queue = arg(ctx, 1); p->pi_created = true;
}
void hook_os_pi_get_cmd_queue(Runtime *rt, MipsContext *ctx) {
    RecompPlatform *p = platform(rt); mips_set(ctx, 2, mips_s32(p->pi_created ? p->pi_queue : 0));
}
void hook_os_pi_start_dma(Runtime *rt, MipsContext *ctx) {
    RecompPlatform *p = platform(rt);
    if (!p->pi_created || arg(ctx, 2) != 0) { result(ctx, -1); return; }
    uint32_t sp = (uint32_t)ctx->gpr[29], mb = arg(ctx, 0), offset = arg(ctx, 3);
    uint32_t address = (uint32_t)recomp_load(rt, sp + 16, 4), size = (uint32_t)recomp_load(rt, sp + 20, 4);
    uint32_t queue = (uint32_t)recomp_load(rt, sp + 24, 4);
    if (!size || !rom_range(rt, &offset, size)) { result(ctx, -1); return; }
    recomp_ram_address(rt, mb, 20); recomp_ram_address(rt, address, size);
    recomp_store(rt, mb, 11, 2); recomp_store(rt, mb + 2, (uint8_t)arg(ctx, 1), 1);
    recomp_store(rt, mb + 4, queue, 4); recomp_store(rt, mb + 8, address, 4);
    recomp_store(rt, mb + 12, arg(ctx, 3), 4); recomp_store(rt, mb + 16, size, 4);
    result(ctx, recomp_queue_send(rt, p->pi_queue, mb, RECOMP_MESG_NOBLOCK, arg(ctx, 1) == 1));
}
void hook_os_pi_raw_read_io(Runtime *rt, MipsContext *ctx) {
    uint32_t offset = arg(ctx, 0);
    if (offset % 4 || !rom_range(rt, &offset, 4)) { result(ctx, -1); return; }
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; i++) value = (value << 8) | rt->rom[offset + i];
    recomp_store(rt, arg(ctx, 1), value, 4); result(ctx, 0);
}
