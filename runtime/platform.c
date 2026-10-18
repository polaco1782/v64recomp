#include "runtime.h"
#include "runtime_debug.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <errno.h>

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
    uint8_t *sram;
};

static RecompPlatform *platform(Runtime *rt) {
    if (!rt->platform) recomp_fail("native platform not initialized", 0);
    return rt->platform;
}
static uint32_t arg(MipsContext *ctx, unsigned index) { return (uint32_t)ctx->gpr[4 + index]; }
static void result(MipsContext *ctx, int32_t value) { mips_set(ctx, 2, mips_s32((uint32_t)value)); }
static void output_ai(Runtime *rt, void *opaque) {
    AiBuffer *buffer=opaque;
    RecompPlatform *p=platform(rt);
    if (p->ai_output.start && !p->ai_output.start(rt, p->ai_output.opaque, buffer->address,
                                               buffer->length, p->ai.clock, buffer->dac_rate))
        recomp_fail("native AI output failed", buffer->address);
}

static void start_ai(Runtime *rt, AiBuffer *buffer, uint64_t start, uint32_t fraction) {
    RecompPlatform *p = platform(rt);
    uint64_t units = (uint64_t)(buffer->length / 4) * recomp_clock_frequency(rt) * buffer->dac_rate + fraction;
    uint64_t ticks = units / p->ai.clock;
    if (ticks >= UINT64_MAX - start) recomp_fail("AI DMA schedule exhausted", buffer->address);
    buffer->start = start; buffer->start_fraction = fraction;
    buffer->end = start + ticks; buffer->end_fraction = (uint32_t)(units % p->ai.clock);
    recomp_on_host(rt,output_ai,buffer);
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
        if (rt->section_loaded[s->section])
            RECOMP_LOG("game", "code section=%" PRIu32 " loaded ram=0x%08" PRIx32 " rom=0x%08" PRIx32 " bytes=%" PRIu32,
                s->section, s->vram, s->rom_offset, s->size);
    }
}

static void service_pi(Runtime *rt) {
    RecompPlatform *p = platform(rt);
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
            uint32_t type = (uint32_t)recomp_load(rt, mb, 2);
            uint32_t handle = type==15 || type==16 ? (uint32_t)recomp_load(rt,mb+20,4) : 0;
            uint32_t base = handle ? (uint32_t)recomp_load(rt,handle+12,4) : UINT32_C(0xb0000000);
            if ((base&UINT32_C(0x1fff0000))==UINT32_C(0x08000000) && p->sram) {
                offset=((base|offset)&UINT32_C(0x1fffffff))-UINT32_C(0x08000000);
                if (!size || offset>p->config.sram_size || size>p->config.sram_size-offset)
                    recomp_fail("SRAM DMA outside cartridge save",mb);
                RECOMP_LOG("save", "SRAM %s offset=0x%08" PRIx32 " ram=0x%08" PRIx32
                    " bytes=%" PRIu32 " path=%s", type==15?"read":"write", offset, address, size,
                    p->config.sram_path?p->config.sram_path:"memory");
                if(type==15)recomp_copy_to_ram(rt,address,p->sram+offset,size);
                else if(type==16) {
                    for(uint32_t b=0;b<size;b++)p->sram[offset+b]=(uint8_t)recomp_load(rt,address+b,1);
                    if(p->config.sram_path) {
                        FILE *save=fopen(p->config.sram_path,"wb");
                        if(!save) {
                            RECOMP_LOG("save", "SRAM open path=%s failed: %s", p->config.sram_path, strerror(errno));
                            recomp_fail("cannot open SRAM save",mb);
                        }
                        bool written=fwrite(p->sram,1,p->config.sram_size,save)==p->config.sram_size;
                        if(fclose(save) || !written) {
                            RECOMP_LOG("save", "SRAM persist path=%s failed", p->config.sram_path);
                            recomp_fail("cannot write SRAM save",mb);
                        }
                        RECOMP_LOG("save", "SRAM persist path=%s bytes=%" PRIu32 " result=ok", p->config.sram_path, p->config.sram_size);
                    }
                } else recomp_fail("unsupported SRAM DMA direction",mb);
                RECOMP_LOG("save", "SRAM transfer complete");
            } else {
                if ((type != 11 && type != 15) || base!=UINT32_C(0xb0000000)
                        || !size || !rom_range(rt, &offset, size))
                    recomp_fail("unsupported/corrupt queued PI transfer", mb);
                recomp_copy_to_ram(rt, address, rt->rom + offset, size);
                code_loaded(rt, address, offset, size);
                RECOMP_LOG_SAMPLED("dma", "ROM read offset=0x%08" PRIx32 " ram=0x%08" PRIx32 " bytes=%" PRIu32 " completion_queue=0x%08" PRIx32,
                    offset, address, size, queue);
            }
            if (queue) (void)recomp_queue_send(rt, queue, mb, RECOMP_MESG_NOBLOCK, false);
        }
    }
}

typedef struct PiSubmission {
    uint32_t message;
    int32_t result;
} PiSubmission;

static void submit_pi_on_host(Runtime *rt, void *opaque) {
    PiSubmission *submission = opaque;
    uint32_t mb = submission->message;
    submission->result = recomp_queue_send(rt, platform(rt)->pi_queue, mb, RECOMP_MESG_NOBLOCK,
                                          recomp_load(rt, mb + 2, 1) == 1);
    /* The native service replaces the high-priority PI manager. Run it before
     * returning to guest code: waiting until a scheduler checkpoint can let an
     * SP task snapshot sample buffers whose ROM transfers are still queued.
     * Enqueue and service on the owner so queue wakeups cannot preempt between
     * them or reenter the service while it is updating a transfer. */
    if (!submission->result) service_pi(rt);
}

static int32_t submit_pi(Runtime *rt, uint32_t mb) {
    PiSubmission submission = {.message = mb};
    recomp_on_host(rt, submit_pi_on_host, &submission);
    return submission.result;
}

static void poll_platform(Runtime *rt) {
    RecompPlatform *p = platform(rt);
    uint64_t now = recomp_count64(rt);
    if (p->initialized) poll_ai(rt);
    service_pi(rt);
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
    /* A host stall (such as reloading textures) must not replay a queue full
     * of old VI notifications before pending SP/DP completions are delivered.
     * Those completions share the game's scheduler queue and can be lost,
     * leaving the guest waiting forever. Coalesce overdue notifications while
     * retaining the elapsed retraces and the requested retrace-count phase. */
    if (p->vi_queue && deliveries)
        (void)recomp_queue_send(rt, p->vi_queue, p->vi_message, RECOMP_MESG_NOBLOCK, false);
}

bool recomp_platform_init(Runtime *rt, const RecompPlatformConfig *config) {
    if (rt->platform || !rt->clock || !rt->scheduler || !rt->rom || !config || !config->vi_period_ticks
            || (config->code_load_count && !config->code_loads)
            || (config->sram_size && config->sram_size!=32768)) return false;
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
    if(config->sram_size) {
        p->sram=malloc(config->sram_size);
        if(!p->sram) { free(p->code_received);free(p);return false; }
        memset(p->sram,0xff,config->sram_size);
        if(config->sram_path) {
            FILE *save=fopen(config->sram_path,"rb");
            bool valid=true;
            if(save) {
                valid=fread(p->sram,1,config->sram_size,save)==config->sram_size && fgetc(save)==EOF;
                if(fclose(save))valid=false;
                RECOMP_LOG("save", "SRAM load path=%s bytes=%" PRIu32 " result=%s", config->sram_path, config->sram_size, valid?"ok":"invalid image/read failure");
            } else {
                valid=errno==ENOENT;
                RECOMP_LOG("save", "SRAM load path=%s result=%s", config->sram_path, valid?"new image (initial=0xff)":strerror(errno));
            }
            if(!valid) { free(p->sram);free(p->code_received);free(p);return false; }
        }
        RECOMP_LOG("save", "SRAM configured bytes=%" PRIu32 " path=%s", config->sram_size, config->sram_path?config->sram_path:"memory");
    }
    p->vi.retrace_count = 1;
    rt->platform = p; rt->destroy_platform = recomp_platform_destroy; rt->poll_platform = poll_platform;
    return true;
}

void recomp_platform_destroy(Runtime *rt) {
    if (!rt->platform) return;
    free(rt->platform->sram);free(rt->platform->code_received); free(rt->platform);
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
    RECOMP_TRACE_OS(rt, ctx);
    RecompAiState state = recomp_ai_state(rt); mips_set(ctx, 2, state.length);
}
void hook_os_ai_get_status(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    RecompAiState state = recomp_ai_state(rt);
    mips_set(ctx, 2, mips_s32((state.queued ? UINT32_C(0x40000000) : 0)
                 | (state.queued == 2 ? UINT32_C(0x80000000) : 0)));
}
void hook_os_ai_set_next_buffer(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
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
    RECOMP_TRACE_OS(rt, ctx);
    RecompPlatform *p = platform(rt);
    if (!p->initialized) recomp_fail("AI frequency before OS initialization", ctx->pc);
    poll_ai(rt);
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
    if (p->ai.queued && dac != p->ai.dac_rate)
        recomp_fail("AI frequency changes during DMA require explicit support", ctx->pc);
    p->ai.dac_rate = dac; p->ai.bit_rate = bit_rate; p->ai.frequency = clock / dac;
    p->ai.enabled = true; result(ctx, (int32_t)p->ai.frequency);
}

void hook_os_initialize(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
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
void hook_os_get_mem_size(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    result(ctx, (int32_t)rt->rdram_size);
}
void hook_leo_drive_exist(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    (void)rt; result(ctx, 0); /* No 64DD device is attached to the native platform. */
}
static void rom_handle(Runtime *rt, MipsContext *ctx, uint32_t handle, bool drive) {
    if (!handle) recomp_fail("SDK ROM handle storage not configured", ctx->pc);
    if (!recomp_load(rt, handle + 12, 4)) {
        recomp_ram_address(rt, handle, 0x74);
        recomp_store(rt, handle + 4, drive ? 1 : 0, 1);
        recomp_store(rt, handle + 5, rt->rom[3], 1);
        recomp_store(rt, handle + 6, rt->rom[1] & 15, 1);
        recomp_store(rt, handle + 7, rt->rom[1] >> 4, 1);
        recomp_store(rt, handle + 8, rt->rom[2], 1);
        recomp_store(rt, handle + 12, drive ? 0xa6000000 : 0xb0000000, 4);
    }
    result(ctx, (int32_t)handle);
}
void hook_os_cart_rom_init(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    rom_handle(rt, ctx, platform(rt)->config.cart_handle, false);
}
void hook_os_drive_rom_init(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    rom_handle(rt, ctx, platform(rt)->config.drive_handle, true);
}
void hook_os_epi_read_io(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    uint32_t handle=arg(ctx, 0), offset=arg(ctx, 1);
    if (recomp_load(rt, handle+12, 4) != UINT32_C(0xb0000000)
            || offset % 4 || !rom_range(rt, &offset, 4)) { result(ctx, -1); return; }
    uint32_t value=0;
    for (unsigned i=0; i<4; i++) value=(value<<8)|rt->rom[offset+i];
    recomp_store(rt, arg(ctx, 2), value, 4); result(ctx, 0);
}
void hook_os_epi_start_dma(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    RecompPlatform *p=platform(rt);
    uint32_t handle=arg(ctx, 0), mb=arg(ctx, 1);
    uint32_t base=(uint32_t)recomp_load(rt,handle+12,4), direction=arg(ctx,2);
    bool sram=(base&UINT32_C(0x1fff0000))==UINT32_C(0x08000000) && p->sram;
    if (!p->pi_created || direction>1 || (direction && !sram)
            || (!sram && base!=UINT32_C(0xb0000000))) { result(ctx, -1); return; }
    recomp_ram_address(rt, mb, 24);
    recomp_store(rt, mb, 15+direction, 2); recomp_store(rt, mb+20, handle, 4);
    result(ctx, submit_pi(rt, mb));
}
void hook_os_create_vi_manager(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    (void)ctx;
    RecompPlatform *p = platform(rt);
    if (p->vi.manager_created) return;
    uint64_t now = recomp_count64(rt);
    if (p->config.vi_period_ticks > UINT64_MAX - now) recomp_fail("VI schedule exhausted", 0);
    p->vi.manager_created = true; p->vi.black = true;
    p->next_vi = now + p->config.vi_period_ticks;
}
void hook_os_vi_set_mode(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    RecompPlatform *p = platform(rt);
    recomp_ram_address(rt, arg(ctx, 0), 80);
    p->vi.mode = arg(ctx, 0);
    /* osViSetMode assigns MODE_UPDATED to the next context's state, clearing
     * the BLACK bit set by manager initialization or an earlier osViBlack. */
    p->vi.black = false;
    p->vi.x_factor = p->vi.y_factor = 1.0f;
    p->mode_ctrl = (uint32_t)recomp_load(rt, p->vi.mode + 4, 4);
    p->vi.special_features = p->mode_ctrl;
}
static float vi_scale(MipsContext *ctx) {
    float value; memcpy(&value, &ctx->fpr[12], sizeof(value));
    if (!isfinite(value) || value<0 || value>16) recomp_fail("invalid VI scale factor", ctx->pc);
    return value;
}
void hook_os_vi_set_x_scale(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    platform(rt)->vi.x_factor=vi_scale(ctx);
}
void hook_os_vi_set_y_scale(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    platform(rt)->vi.y_factor=vi_scale(ctx);
}
void hook_os_vi_black(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    platform(rt)->vi.black = (uint8_t)arg(ctx, 0) != 0;
}
void hook_os_vi_set_special_features(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
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
    RECOMP_TRACE_OS(rt, ctx);
    RecompPlatform *p = platform(rt);
    p->vi_queue = arg(ctx, 0); p->vi_message = arg(ctx, 1);
    p->vi.retrace_count = (uint16_t)arg(ctx, 2); p->retrace_phase = 0;
}
void hook_os_vi_swap_buffer(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    /* The SDK records the pending pointer even while VI output is black. */
    platform(rt)->vi.framebuffer = arg(ctx, 0);
}
void hook_os_vi_get_current_framebuffer(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    result(ctx, (int32_t)platform(rt)->vi.current_framebuffer);
}
void hook_os_vi_get_next_framebuffer(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    result(ctx, (int32_t)platform(rt)->vi.framebuffer);
}
void hook_os_create_pi_manager(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    RecompPlatform *p = platform(rt);
    if (p->pi_created) return;
    recomp_queue_create(rt, arg(ctx, 1), arg(ctx, 2), (int32_t)mips_i32(ctx->gpr[7]));
    p->pi_queue = arg(ctx, 1); p->pi_created = true;
}
void hook_os_pi_get_cmd_queue(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    RecompPlatform *p = platform(rt); mips_set(ctx, 2, mips_s32(p->pi_created ? p->pi_queue : 0));
}
void hook_os_pi_start_dma(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
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
    result(ctx, submit_pi(rt, mb));
}
void hook_os_pi_raw_read_io(Runtime *rt, MipsContext *ctx) {
    RECOMP_TRACE_OS(rt, ctx);
    uint32_t offset = arg(ctx, 0);
    if (offset % 4 || !rom_range(rt, &offset, 4)) { result(ctx, -1); return; }
    uint32_t value = 0;
    for (unsigned i = 0; i < 4; i++) value = (value << 8) | rt->rom[offset + i];
    recomp_store(rt, arg(ctx, 1), value, 4); result(ctx, 0);
}
