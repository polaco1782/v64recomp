#include "audio_internal.h"
#include "timing.h"
#include "hle_external.h"
#include "ucodes.h"
#include "memory.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static void dmem_range(unsigned address, unsigned bytes) {
    if (address > 4096 || bytes > 4096 - address)
        recomp_fail("audio DMEM range/alignment", address);
}
static void sample_range(unsigned address, unsigned bytes) {
    if (address & 1)
        recomp_fail("audio sample alignment", address);
    dmem_range(address, bytes);
}
static uint32_t ram_range(RecompAudio *a, uint32_t address, size_t bytes, unsigned alignment) {
    if (address % alignment)
        recomp_fail("audio RAM alignment", address);
    return recomp_physical_address(a->rt, address, bytes);
}
void HleMusyxValidateRam(void *opaque, uint32_t address, size_t bytes, unsigned alignment) {
    RecompAudio *a = opaque;
    if (address % alignment)
        recomp_fail("MusyX RAM alignment", address);
    uint32_t physical = recomp_ram_address(a->rt, address, bytes);
    if (physical != (address & UINT32_C(0xffffff)))
        recomp_fail("MusyX address cannot use the native HLE RAM window", address);
}
void HleVerboseMessage(void *opaque, const char *message, ...) {
    (void)opaque;
    (void)message;
}
static uint32_t address(RecompAudio *a, uint32_t word) {
    unsigned segment = (word >> 24) & 63;
    if (segment >= 16)
        recomp_fail("audio segment outside table", segment);
    uint64_t result = (uint64_t)a->hle.alist_audio.segments[segment] + (word & 0xffffff);
    if (result > UINT32_MAX)
        recomp_fail("audio segment address overflow", word);
    return (uint32_t)result;
}
static void skip_audio_task(RecompAudio *a, const char *message, uint32_t pc) {
    if (!a->hle_active)
        recomp_fail(message, pc);
    recomp_warn(message, pc);
    longjmp(a->skip_task, 1);
}
void HleWarnMessage(void *opaque, const char *message, ...) {
    RecompAudio *a = opaque;
    char detail[256], warning[288];
    va_list args;
    va_start(args, message);
    vsnprintf(detail, sizeof(detail), message, args);
    va_end(args);
    snprintf(warning, sizeof(warning), "audio HLE: %s", detail);
    uint32_t pc = a->debug.command_index ? a->debug.history[(a->debug.command_index - 1) % RECOMP_AUDIO_HISTORY].address
                                         : a->debug.list;
    skip_audio_task(a, warning, pc);
}
int HleForwardTask(void *opaque) {
    HleWarnMessage(opaque, "unsupported forwarded audio microcode");
    return 0;
}
void rsp_break(struct hle_t *hle, unsigned flags) {
    RecompAudio *a = hle->user_defined;
    if (flags != SP_STATUS_TASKDONE || a->task_done)
        recomp_fail("invalid audio task completion", flags);
    a->task_done = true;
}
static void validate_nead(RecompAudio *a, uint32_t w1, uint32_t w2, uint32_t pc) {
    struct alist_nead_t *s = &a->hle.alist_nead;
    unsigned op = w1 >> 24, flags = (w1 >> 16) & 255, count = s->count;
    unsigned lo = w1 & 65535, left = w2 >> 16, right = w2 & 65535, addr = w2 & 0xffffff;
    if (op >= 24 || op == 0 || (op == 3 && a->microcode != RECOMP_AUDIO_NEAD_MM) || op == 23)
        skip_audio_task(a, "unknown Zelda audio command", pc);
    a->stats.commands++;
    a->stats.opcodes[op]++;
    switch (op) {
        case 1:
            if (flags & ~7u || (count && a->book_coefficients < 16))
                recomp_fail("invalid Zelda ADPCM setup", pc);
            dmem_range(s->in, (align(count, 32) / 32) * ((flags & 4) ? 5 : 9));
            sample_range(s->out, align(count, 32) + 32);
            ram_range(a, addr, 32, 2);
            if (flags & 2)
                ram_range(a, s->loop, 32, 2);
            break;
        case 2:
            dmem_range(lo, w2 & 4095);
            break;
        case 4:
        case 12:
            sample_range(left, (w1 >> 12) & 0xff0);
            sample_range(right, (w1 >> 12) & 0xff0);
            break;
        case 5:
        case 6: {
            unsigned n = op == 5 ? align(count, 16) : count;
            if (op == 5 && (flags & ~1u || s->in < 8))
                recomp_fail("invalid Zelda resample setup", pc);
            sample_range(s->out, n);
            unsigned samples = n / 2, history = op == 5 ? 8 : 0;
            uint64_t span = ((samples * (uint64_t)lo * 2 + 65535) / 65536 + 4) * 2;
            if (span > 4096)
                recomp_fail("Zelda resample span", pc);
            sample_range(s->in - history, (unsigned)span + history);
            if (op == 5)
                ram_range(a, addr, 10, 2);
            break;
        }
        case 7:
            if (flags > 1) {
                if (lo > 0x780)
                    recomp_fail("Zelda filter count", pc);
                ram_range(a, addr, 16, 2);
            } else {
                if (s->filter_count > 0x780 || s->filter_count % 16)
                    recomp_fail("Zelda filter setup", pc);
                sample_range(lo, s->filter_count);
                ram_range(a, addr, 32, 2);
                ram_range(a, s->filter_lut_address[0], 16, 2);
            }
            break;
        case 8:
            dmem_range(lo, 0);
            dmem_range(left, 0);
            break;
        case 9:
            dmem_range(lo, 128);
            dmem_range(left, flags * 128);
            break;
        case 10:
            dmem_range(lo, align(right, 4));
            dmem_range(left, align(right, 4));
            break;
        case 11:
            if (lo > sizeof(s->table) || lo % 32)
                recomp_fail("Zelda ADPCM book size", pc);
            ram_range(a, addr, lo, 2);
            a->book_coefficients = lo / 2;
            break;
        case 13: {
            unsigned n = (w1 >> 12) & 0xff0;
            sample_range(lo, n * 2);
            sample_range(left, n);
            sample_range(right, n);
            break;
        }
        case 14:
            sample_range(left, w1 & 4095);
            break;
        case 15:
            ram_range(a, addr, 32, 2);
            break;
        case 16: {
            unsigned n = (flags ? flags : 1) * align(right ? right : 32, 32);
            dmem_range(lo, n);
            dmem_range(left, n);
            break;
        }
        case 17:
            sample_range(left, lo * 4);
            sample_range(right, lo * 2);
            break;
        case 18:
        case 22:
            break;
        case 19: {
            unsigned bytes = align((w1 >> 8) & 255, 8) * 2;
            sample_range((w1 >> 12) & 0xff0, bytes);
            sample_range((w2 >> 20) & 0xff0, bytes);
            sample_range((w2 >> 12) & 0xff0, bytes);
            sample_range((w2 >> 4) & 0xff0, bytes);
            sample_range((w2 << 4) & 0xff0, bytes);
            break;
        }
        case 20:
        case 21: {
            unsigned bytes = align((w1 >> 12) & 0xfff, 8);
            dmem_range((w1 & 4095) & ~3u, bytes);
            ram_range(a, addr & ~7u, bytes, 8);
            break;
        }
    }
}
static void validate_naudio(RecompAudio *a, uint32_t w1, uint32_t w2, uint32_t pc) {
    unsigned op = w1 >> 24, addr = w2 & 0xffffff;
    if (op > 15)
        skip_audio_task(a, "unknown NAudio command", pc);
    bool standard = a->microcode == RECOMP_AUDIO_NAUDIO;
    if (standard && (op == 7 || op == 8))
        skip_audio_task(a, "unsupported standard NAudio command", pc);
    a->stats.commands++;
    a->stats.opcodes[op]++;
    switch (op) {
        case 0:
            if (!standard)
                sample_range(0x4f0 + (w1 & 4095), 0x170);
            break;
        case 1: {
            unsigned n = align((w2 >> 16) & 4095, 32);
            if (n && a->book_coefficients < 16)
                recomp_fail("NAudio ADPCM without codebook", pc);
            dmem_range(0x4f0 + ((w2 >> 12) & 15), n / 32 * 9);
            sample_range(0x4f0 + (w2 & 4095), n + 32);
            ram_range(a, w1 & 0xffffff, 32, 2);
            if (w2 & 0x20000000)
                ram_range(a, a->hle.alist_naudio.loop, 32, 2);
            break;
        }
        case 2:
            dmem_range(0x4f0 + (w1 & 65535), w2 & 4095);
            break;
        case 3:
            ram_range(a, addr, 80, 4);
            break;
        case 4:
        case 6: {
            unsigned n = (w1 >> 12) & 4095;
            dmem_range((0x4f0 + (w1 & 4095)) & ~3u, align(n, 8));
            ram_range(a, addr & ~7u, align(n, 8), 8);
            break;
        }
        case 5: {
            unsigned input = 0x4f0 + ((w2 >> 2) & 4095), pitch = ((w2 >> 14) & 65535) * 2;
            sample_range(input - 8, ((184 * (uint64_t)pitch + 65535) / 65536 + 4) * 2 + 8);
            ram_range(a, w1 & 0xffffff, 10, 2);
            break;
        }
        case 7:
            ram_range(a, addr, 0x488, 2);
            break;
        case 8:
        case 9:
        case 13:
            break;
        case 10:
            dmem_range(0x4f0 + (w1 & 65535), align(w2 & 65535, 4));
            dmem_range(0x4f0 + (w2 >> 16), align(w2 & 65535, 4));
            break;
        case 11: {
            unsigned n = w1 & 65535;
            if (n > sizeof(a->hle.alist_naudio.table) || n % 32)
                recomp_fail("NAudio codebook size", pc);
            ram_range(a, addr, n, 2);
            a->book_coefficients = n / 2;
            break;
        }
        case 12:
            sample_range(0x4f0 + (w2 >> 16), 0x170);
            sample_range(0x4f0 + (w2 & 65535), 0x170);
            break;
        case 14:
            if (!standard)
                ram_range(a, addr, 32, 2);
            break;
        case 15:
            ram_range(a, addr, 32, 2);
            break;
    }
}
#ifdef RECOMP_DEBUG
/* The HLE validator sees the decoded segment bases and DMEM buffer selections
 * before each command. Compare final RDRAM for each distinct output span. */
static void track_audio_output(RecompAudio *a, uint32_t addr, unsigned bytes, bool pcm) {
    if (!bytes)
        return;
    for (size_t i = 0; i < a->span_count; i++)
        if (a->spans[i].address == addr && a->spans[i].bytes == bytes && a->spans[i].pcm == pcm)
            return;
    if (a->span_count == a->span_capacity) {
        size_t capacity = a->span_capacity ? a->span_capacity * 2 : 32;
        struct AudioSpan *spans = realloc(a->spans, capacity * sizeof(*spans));
        if (!spans)
            recomp_fail("audio comparison allocation failed", addr);
        a->span_capacity = capacity;
        a->spans = spans;
    }
    a->spans[a->span_count++] = (struct AudioSpan){addr, bytes, pcm};
}
static void report_audio_comparison(RecompAudio *a) {
    unsigned pcm = 0, changed = 0, peak_delta = 0, nonzero = 0, history = 0, history_changed = 0;
    for (size_t i = 0; i < a->span_count; i++) {
        struct AudioSpan span = a->spans[i];
        for (unsigned j = 0; j < span.bytes;) {
            uint32_t addr = span.address + j;
            if (span.pcm) {
                int16_t reference, compiled;
                /* Native-word RAM can reverse the two halfwords in each word.
                 * Compare the same lane in both aligned PCM buffers. */
                memcpy(&reference, a->rt->rdram + addr, 2);
                memcpy(&compiled, a->compare_ram + addr, 2);
                unsigned delta = (unsigned)abs((int)reference - compiled);
                pcm++;
                nonzero += reference != 0;
                changed += delta != 0;
                if (delta > peak_delta)
                    peak_delta = delta;
                j += 2;
            } else {
#if RECOMP_LITTLE_ENDIAN_HOST
                addr ^= 3;
#endif
                history++;
                history_changed += a->rt->rdram[addr] != a->compare_ram[addr];
                j++;
            }
        }
    }
    fprintf(stderr,
            "Audio RSP comparison task=%" PRIu64 " pcm_samples=%u nonzero_hle=%u pcm_differences=%u "
            "max_delta=%u adpcm_history_bytes=%u history_differences=%u\n",
            a->stats.tasks, pcm, nonzero, changed, peak_delta, history, history_changed);
}
#endif
void HleValidateCommand(void *opaque, uint32_t w1, uint32_t w2, uint32_t pc) {
    RecompAudio *a = opaque;
    struct alist_audio_t *s = &a->hle.alist_audio;
    recomp_audio_record_command(a, w1, w2, pc);
    if (a->microcode == RECOMP_AUDIO_NEAD_OOT || a->microcode == RECOMP_AUDIO_NEAD_MM ||
        a->microcode == RECOMP_AUDIO_NEAD_1080) {
        validate_nead(a, w1, w2, pc);
        return;
    }
    if (a->microcode == RECOMP_AUDIO_NAUDIO || a->microcode == RECOMP_AUDIO_NAUDIO_MP3 ||
        a->microcode == RECOMP_AUDIO_NAUDIO_CBFD) {
        validate_naudio(a, w1, w2, pc);
        return;
    }
    unsigned op = w1 >> 24, flags = (w1 >> 16) & 255, count = s->count;
    if (op > 15)
        skip_audio_task(a, "unknown ABI1 audio command", pc);
#ifdef RECOMP_DEBUG
    /* Both compiled ABI1 programs save interleaved PCM with SETBUFF/SAVEBUFF.
     * Later commands reuse the same DMEM for effects and decoder scratch. */
    if (op != 8 && op != 6)
        a->interleave_end = 0;
#endif
    a->stats.commands++;
    a->stats.opcodes[op]++;
    switch (op) {
        case 0:
        case 9:
            break;
        case 7:
            if (((w2 >> 24) & 63) >= 16)
                recomp_fail("audio segment index", pc);
            break;
        case 8:
            dmem_range(0x5c0 + (w1 & 65535), 0);
            dmem_range(0x5c0 + (w2 >> 16), 0);
            if (flags & 8)
                dmem_range(0x5c0 + (w2 & 65535), 0);
            break;
        case 2:
            dmem_range(0x5c0 + (w1 & 65535), align(w2 & 0xfff, 16));
            break;
        case 10:
            dmem_range(0x5c0 + (w1 & 65535), align(w2 & 65535, 16));
            dmem_range(0x5c0 + (w2 >> 16), align(w2 & 65535, 16));
            break;
        case 11: {
            unsigned bytes = align(w1 & 65535, 8);
            if (bytes > sizeof(s->table) || bytes % 32)
                recomp_fail("audio codebook size", pc);
            ram_range(a, address(a, w2), bytes, 2);
            a->book_coefficients = bytes / 2;
            break;
        }
        case 4:
        case 6:
            dmem_range((op == 4 ? s->in : s->out) & ~3u, align(count, 8));
            ram_range(a, address(a, w2) & ~7u, align(count, 8), 8);
#ifdef RECOMP_DEBUG
            if (a->compare_ram && op == 6 && (s->out & ~3u) >= a->interleave_start &&
                (s->out & ~3u) + align(count, 8) <= a->interleave_end)
                track_audio_output(a, address(a, w2) & ~7u, align(count, 8), true);
#endif
            break;
        case 12:
            sample_range(0x5c0 + (w2 >> 16), align(count, 32));
            sample_range(0x5c0 + (w2 & 65535), align(count, 32));
            break;
        case 13:
            sample_range(0x5c0 + (w2 >> 16), align(count, 16));
            sample_range(0x5c0 + (w2 & 65535), align(count, 16));
            sample_range(s->out, align(count, 16) * 2);
#ifdef RECOMP_DEBUG
            if (a->compare_ram) {
                a->interleave_start = s->out;
                a->interleave_end = s->out + align(count, 16) * 2;
            }
#endif
            break;
        case 15:
            ram_range(a, address(a, w2), 32, 2);
            break;
        case 1:
            if (flags & ~3u)
                skip_audio_task(a, "unsupported ABI1 ADPCM flags", pc);
            if (count && a->book_coefficients < 16)
                recomp_fail("ADPCM without a codebook", pc);
            dmem_range(s->in, (align(count, 32) / 32) * 9);
            sample_range(s->out, align(count, 32) + 32);
            ram_range(a, address(a, w2), 32, 2);
#ifdef RECOMP_DEBUG
            if (a->compare_ram)
                track_audio_output(a, address(a, w2), 32, false);
#endif
            if (flags & 2)
                ram_range(a, s->loop, 32, 2);
            break;
        case 5: {
            if (flags & ~1u)
                skip_audio_task(a, "unsupported ABI1 resampling flags", pc);
            unsigned samples = align(count, 16) / 2, pitch = (w1 & 65535) * 2;
            if (s->in < 8)
                recomp_fail("audio resampling history range", pc);
            sample_range(s->out, samples * 2);
            sample_range(s->in - 8, ((samples * (uint64_t)pitch + 65535) / 65536 + 4) * 2);
            ram_range(a, address(a, w2), 10, 2);
            break;
        }
        case 3:
            if (flags & ~9u)
                skip_audio_task(a, "unsupported ABI1 envelope flags", pc);
            sample_range(s->in, align(count, 16));
            sample_range(s->out, align(count, 16));
            sample_range(s->dry_right, align(count, 16));
            if (flags & 8) {
                sample_range(s->wet_left, align(count, 16));
                sample_range(s->wet_right, align(count, 16));
            }
            ram_range(a, address(a, w2), 80, 4);
            break;
        case 14:
            sample_range(s->in, align(count, 16));
            sample_range(s->out, align(count, 16));
            ram_range(a, address(a, w2), 8, 4);
            break;
    }
}
static void execute_abi1(RecompAudio *a, uint32_t list, uint32_t bytes) {
    uint32_t physical = recomp_audio_prepare_list(a, list, bytes);
    memcpy(a->dmem + TASK_DATA_PTR, &physical, 4);
    memcpy(a->dmem + TASK_DATA_SIZE, &bytes, 4);
    a->task_done = false;
    uint64_t start = recomp_audio_timer_ns();
    a->hle_active = true;
    if (setjmp(a->skip_task) == 0) {
        if (a->microcode == RECOMP_AUDIO_NEAD_OOT)
            alist_process_nead_oot(&a->hle);
        else if (a->microcode == RECOMP_AUDIO_NEAD_MM)
            alist_process_nead_mm(&a->hle);
        else if (a->microcode == RECOMP_AUDIO_NEAD_1080)
            alist_process_nead_1080(&a->hle);
        else if (a->microcode == RECOMP_AUDIO_NAUDIO)
            alist_process_naudio(&a->hle);
        else if (a->microcode == RECOMP_AUDIO_NAUDIO_MP3)
            alist_process_naudio_mp3(&a->hle);
        else if (a->microcode == RECOMP_AUDIO_NAUDIO_CBFD)
            alist_process_naudio_cbfd(&a->hle);
        else if (a->microcode == RECOMP_AUDIO_ABI1_LINEAR)
            alist_process_audio_ge(&a->hle);
        else
            alist_process_audio(&a->hle);
    } else {
#ifdef RECOMP_DEBUG
        if (a->compare_ram)
            recomp_fail("HLE skipped the audio task selected for comparison", list);
#endif
        a->task_done = true;
    }
    a->hle_active = false;
    uint64_t elapsed = recomp_audio_timer_ns() - start;
    a->stats.synth_ns += elapsed;
    recomp_timing_add(a->rt, RECOMP_TIMING_AUDIO, elapsed, 1);
    if (!a->task_done)
        recomp_fail("audio DSP task did not finish", list);
    a->stats.tasks++;
}
static void execute_musyx(RecompAudio *a, uint32_t list, uint32_t count) {
    if (!count || count > 64)
        recomp_fail("invalid MusyX subframe count", list);
    HleMusyxValidateRam(a, list, (size_t)count * 0xa10, 4);
    uint32_t physical = list & UINT32_C(0xffffff);
    a->debug = (RecompAudioSnapshot){.list = list, .bytes = count * 0xa10};
    memcpy(a->dmem + TASK_DATA_PTR, &physical, 4);
    memcpy(a->dmem + TASK_DATA_SIZE, &count, 4);
    a->task_done = false;
    uint64_t start = recomp_audio_timer_ns();
    a->hle_active = true;
    if (setjmp(a->skip_task) == 0)
        musyx_v1_task(&a->hle);
    else
        a->task_done = true;
    a->hle_active = false;
    uint64_t elapsed = recomp_audio_timer_ns() - start;
    a->stats.synth_ns += elapsed;
    recomp_timing_add(a->rt, RECOMP_TIMING_AUDIO, elapsed, 1);
    if (!a->task_done)
        recomp_fail("MusyX task did not finish", list);
    a->stats.tasks++;
    a->stats.commands += count;
}
void recomp_audio_execute_hle(RecompAudio *a, uint32_t list, uint32_t size) {
    if (a->microcode == RECOMP_AUDIO_MUSYX_V1)
        execute_musyx(a, list, size);
    else
        execute_abi1(a, list, size);
}
#ifdef RECOMP_DEBUG
void recomp_audio_compare_hle(RecompAudio *a, uint32_t list, uint32_t bytes) {
    a->span_count = 0;
    a->interleave_start = a->interleave_end = 0;
    execute_abi1(a, list, bytes);
    report_audio_comparison(a);
}
#endif
