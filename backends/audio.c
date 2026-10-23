#define _POSIX_C_SOURCE 200809L
#include "recomp_audio.h"
#include "runtime_debug.h"
#include "hle_internal.h"
#include "hle_external.h"
#include "ucodes.h"
#include "memory.h"
#include <SDL2/SDL.h>
#include <openssl/evp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

struct RecompAudio {
    Runtime *rt;
    struct hle_t hle;
    _Alignas(16) uint8_t dmem[4096];
    RecompAudioStats stats;
    RecompAudioSnapshot debug;
    FILE *wav;
    FILE *trace;
    SDL_AudioDeviceID device;
    SDL_threadID owner;
    bool playback, enabled, microcode_checked, task_done, sdl_audio;
    unsigned book_coefficients;
    RecompAudioMicrocode microcode;
    const RecompAudioTaskProfile *task_profile;
    unsigned prebuffer_ms;
};
static uint64_t timer_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        abort();
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static void owner(RecompAudio *a) {
    if (a->owner != SDL_ThreadID())
        recomp_fail("audio accessed outside owner thread", 0);
}
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
void HleWarnMessage(void *opaque, const char *message, ...) {
    (void)opaque;
    va_list args;
    va_start(args, message);
    fputs("audio HLE: ", stderr);
    vfprintf(stderr, message, args);
    fputc('\n', stderr);
    va_end(args);
    recomp_fail("unsupported audio HLE operation", 0);
}
int HleForwardTask(void *opaque) {
    (void)opaque;
    recomp_fail("unsupported forwarded audio microcode", 0);
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
    if (op >= 24 || op == 0 || op == 3 || op == 23)
        recomp_fail("unknown Zelda audio command", pc);
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
        recomp_fail("unknown NAudio command", pc);
    bool standard = a->microcode == RECOMP_AUDIO_NAUDIO;
    if (standard && (op == 7 || op == 8))
        recomp_fail("unsupported standard NAudio command", pc);
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
void HleValidateCommand(void *opaque, uint32_t w1, uint32_t w2, uint32_t pc) {
    RecompAudio *a = opaque;
    struct alist_audio_t *s = &a->hle.alist_audio;
    a->debug.history[a->debug.command_index % RECOMP_AUDIO_HISTORY] = (RecompAudioCommand){pc, w1, w2};
    a->debug.command_index++;
    if (a->debug.history_count < RECOMP_AUDIO_HISTORY)
        a->debug.history_count++;
    if (a->microcode == RECOMP_AUDIO_NEAD_OOT || a->microcode == RECOMP_AUDIO_NEAD_1080) {
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
        recomp_fail("unknown ABI1 audio command", pc);
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
            break;
        case 12:
            sample_range(0x5c0 + (w2 >> 16), align(count, 32));
            sample_range(0x5c0 + (w2 & 65535), align(count, 32));
            break;
        case 13:
            sample_range(0x5c0 + (w2 >> 16), align(count, 16));
            sample_range(0x5c0 + (w2 & 65535), align(count, 16));
            sample_range(s->out, align(count, 16) * 2);
            break;
        case 15:
            ram_range(a, address(a, w2), 32, 2);
            break;
        case 1:
            if (flags & ~3u)
                recomp_fail("unsupported ABI1 ADPCM flags", pc);
            if (count && a->book_coefficients < 16)
                recomp_fail("ADPCM without a codebook", pc);
            dmem_range(s->in, (align(count, 32) / 32) * 9);
            sample_range(s->out, align(count, 32) + 32);
            ram_range(a, address(a, w2), 32, 2);
            if (flags & 2)
                ram_range(a, s->loop, 32, 2);
            break;
        case 5: {
            if (flags & ~1u)
                recomp_fail("unsupported ABI1 resampling flags", pc);
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
                recomp_fail("unsupported ABI1 envelope flags", pc);
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
static void little(uint8_t *p, uint32_t v, unsigned n) {
    for (unsigned i = 0; i < n; i++)
        p[i] = (uint8_t)(v >> (i * 8));
}
static bool wav_header(RecompAudio *a) {
    if (!a->wav)
        return true;
    if (a->stats.frames > (UINT32_MAX - 36) / 4)
        return false;
    uint8_t h[44] = {0};
    memcpy(h, "RIFF", 4);
    little(h + 4, 36 + (uint32_t)a->stats.frames * 4, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    little(h + 16, 16, 4);
    little(h + 20, 1, 2);
    little(h + 22, 2, 2);
    little(h + 24, (a->stats.frequency ? a->stats.frequency : 32000), 4);
    little(h + 28, (a->stats.frequency ? a->stats.frequency : 32000) * 4, 4);
    little(h + 32, 4, 2);
    little(h + 34, 16, 2);
    memcpy(h + 36, "data", 4);
    little(h + 40, (uint32_t)a->stats.frames * 4, 4);
    return !fseek(a->wav, 0, SEEK_SET) && fwrite(h, 1, 44, a->wav) == 44;
}
static bool open_device(RecompAudio *a, unsigned frequency) {
    if (a->device)
        return true;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr, "SDL audio initialization: %s\n", SDL_GetError());
        return false;
    }
    a->sdl_audio = true;
    SDL_AudioSpec want = {0}, have = {0};
    want.freq = (int)frequency;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    a->device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!a->device) {
        fprintf(stderr, "SDL audio device: %s\n", SDL_GetError());
        return false;
    }
    if (have.freq != want.freq || have.format != want.format || have.channels != want.channels)
        return false;
    /* SDL opens paused. Accumulate a reserve before starting the device so
     * owner-thread synthesis/rendering jitter does not immediately run it dry. */
    a->stats.prebuffer_frames = frequency * a->prebuffer_ms / 1000;
    if (a->stats.prebuffer_frames < 3u * have.samples)
        a->stats.prebuffer_frames = 3u * have.samples;
    a->stats.device_open = true;
    snprintf(a->stats.driver, sizeof(a->stats.driver), "%s", SDL_GetCurrentAudioDriver());
    if (a->trace)
        fprintf(stderr, "Audio trace: driver=%s rate=%d device_period=%u prebuffer_frames=%u\n", a->stats.driver,
                have.freq, have.samples, a->stats.prebuffer_frames);
    return true;
}
static bool ai_start(Runtime *rt, void *opaque, uint32_t guest, unsigned bytes, unsigned clock, unsigned divisor) {
    RecompAudio *a = opaque;
    owner(a);
    (void)rt;
    if (!a->enabled)
        return true;
    unsigned rate = clock / divisor;
    if (a->stats.frequency && a->stats.frequency != rate)
        recomp_fail("audio capture/playback rate changed", rate);
    a->stats.frequency = rate;
    int16_t *pcm = malloc(bytes);
    uint8_t *wav = a->wav ? malloc(bytes) : NULL;
    if (!pcm || (a->wav && !wav)) {
        free(pcm);
        free(wav);
        return false;
    }
    for (unsigned i = 0; i < bytes / 2; i++) {
        pcm[i] = (int16_t)recomp_load(a->rt, guest + i * 2, 2);
        unsigned magnitude = pcm[i] < 0 ? (unsigned)-(int)pcm[i] : (unsigned)pcm[i];
        if (magnitude > a->stats.peak)
            a->stats.peak = magnitude;
        a->stats.nonzero_samples += pcm[i] != 0;
        a->stats.clipped_samples += magnitude >= 32767;
        if (wav)
            little(wav + i * 2, (uint16_t)pcm[i], 2);
    }
    bool okay = true;
    if (a->wav)
        okay = fwrite(wav, 1, bytes, a->wav) == bytes;
    if (a->playback) {
        okay = okay && open_device(a, rate);
        if (okay) {
            unsigned queued = SDL_GetQueuedAudioSize(a->device);
            if (a->stats.playback_running) {
                if (queued < a->stats.min_queued_bytes)
                    a->stats.min_queued_bytes = queued;
                if (!queued) {
                    a->stats.queue_underruns++;
                    SDL_PauseAudioDevice(a->device, 1);
                    a->stats.playback_running = false;
                }
            }
            /* Bound latency if the device stalls; never accumulate seconds of old sound. */
            if (queued > rate * 4 / 4) {
                SDL_PauseAudioDevice(a->device, 1);
                a->stats.playback_running = false;
                SDL_ClearQueuedAudio(a->device);
                a->stats.queue_resets++;
            }
            okay = SDL_QueueAudio(a->device, pcm, bytes) == 0;
            if (okay) {
                a->stats.playback_frames += bytes / 4;
                queued = SDL_GetQueuedAudioSize(a->device);
                if (queued > a->stats.max_queued_bytes)
                    a->stats.max_queued_bytes = queued;
                if (!a->stats.playback_running && queued >= a->stats.prebuffer_frames * 4) {
                    SDL_PauseAudioDevice(a->device, 0);
                    a->stats.playback_running = true;
                }
            }
        }
    }
    a->stats.frames += bytes / 4;
    a->stats.ai_buffers++;
    if (a->trace)
        fprintf(a->trace, "%" PRIu64 ",%u,%u,%u,%" PRIu64 "\n", timer_ns(), bytes / 4, rate,
                a->device ? SDL_GetQueuedAudioSize(a->device) : 0, a->stats.queue_underruns);
    free(pcm);
    free(wav);
    return okay;
}
#ifdef RECOMP_DEBUG
static void debug_audio_snapshot(void *opaque, RecompDebugAudioStats *out) {
    RecompAudio *a = opaque;
    owner(a);
    /* Synthesis and this snapshot run on the owner with guest execution parked. */
    *out = (RecompDebugAudioStats){.tasks = a->stats.tasks,
                                   .synth_ns = a->stats.synth_ns,
                                   .underruns = a->stats.queue_underruns,
                                   .available = true,
                                   .playback = a->playback && a->enabled,
                                   .device_open = a->device && a->stats.frequency,
                                   .queue_ms = a->device && a->stats.frequency ? SDL_GetQueuedAudioSize(a->device) *
                                                                                     1000.0 / (a->stats.frequency * 4.0)
                                                                               : 0.0};
}
#endif
RecompAudio *recomp_audio_create(Runtime *rt, const RecompAudioConfig *config) {
    if (!rt || !rt->platform || !config || config->prebuffer_ms > 200 ||
        (config->microcode != RECOMP_AUDIO_ABI1 && config->microcode != RECOMP_AUDIO_ABI1_LINEAR &&
         config->microcode != RECOMP_AUDIO_NEAD_OOT && config->microcode != RECOMP_AUDIO_NAUDIO_MP3 &&
         config->microcode != RECOMP_AUDIO_NAUDIO_CBFD && config->microcode != RECOMP_AUDIO_NAUDIO &&
         config->microcode != RECOMP_AUDIO_MUSYX_V1 && config->microcode != RECOMP_AUDIO_NEAD_1080))
        return NULL;
    RecompAudio *a = calloc(1, sizeof(*a));
    if (!a)
        return NULL;
    a->rt = rt;
    a->owner = SDL_ThreadID();
    a->playback = config->playback;
    a->enabled = config->output_enabled;
    a->microcode = config->microcode;
    a->task_profile = config->task_profile;
    a->prebuffer_ms = config->prebuffer_ms ? config->prebuffer_ms : 80;
    a->stats.min_queued_bytes = UINT32_MAX;
    a->hle.dram = rt->rdram;
    a->hle.dmem = a->dmem;
    a->hle.user_defined = a;
    if (config->wav_path) {
        a->wav = fopen(config->wav_path, "wb+");
        if (!a->wav || !wav_header(a) || fseek(a->wav, 44, SEEK_SET)) {
            if (a->wav)
                fclose(a->wav);
            free(a);
            return NULL;
        }
    }
    const char *trace_path = config->trace_path ? config->trace_path : getenv("RECOMP_AUDIO_TRACE");
    if (trace_path && *trace_path) {
        a->trace = fopen(trace_path, "w");
        if (!a->trace) {
            if (a->wav)
                fclose(a->wav);
            free(a);
            return NULL;
        }
        fputs("host_ns,frames,rate,queued_bytes,underruns\n", a->trace);
    }
    RecompAiOutput output = {.start = ai_start, .opaque = a};
    if (!recomp_ai_set_output(rt, &output)) {
        if (a->trace)
            fclose(a->trace);
        if (a->wav)
            fclose(a->wav);
        free(a);
        return NULL;
    }
#ifdef RECOMP_DEBUG
    rt->debug_audio_userdata = a;
    rt->debug_audio_snapshot = debug_audio_snapshot;
#endif
    return a;
}
void recomp_audio_destroy(RecompAudio *a) {
    if (!a)
        return;
    owner(a);
#ifdef RECOMP_DEBUG
    if (a->rt->debug_audio_userdata == a) {
        a->rt->debug_audio_snapshot = NULL;
        a->rt->debug_audio_userdata = NULL;
    }
#endif
    (void)recomp_ai_set_output(a->rt, NULL);
    if (a->device)
        SDL_CloseAudioDevice(a->device);
    if (a->sdl_audio)
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
    if (a->wav && (!wav_header(a) || fclose(a->wav)))
        recomp_fail("audio WAV finalization failed", 0);
    if (a->trace) {
        fprintf(stderr,
                "Audio trace: buffers=%" PRIu64 " frames=%" PRIu64 " underruns=%" PRIu64 " resets=%" PRIu64 "\n",
                a->stats.ai_buffers, a->stats.frames, a->stats.queue_underruns, a->stats.queue_resets);
        if (fclose(a->trace))
            recomp_fail("audio trace finalization failed", 0);
    }
    free(a);
}
void recomp_audio_set_output_enabled(RecompAudio *a, bool enabled) {
    owner(a);
    if (enabled && a->playback && !a->device) {
        RecompAiState state = recomp_ai_state(a->rt);
        if (state.frequency && !open_device(a, state.frequency))
            recomp_fail("SDL audio device unavailable", 0);
    }
    if (!enabled && a->device) {
        SDL_PauseAudioDevice(a->device, 1);
        a->stats.playback_running = false;
        SDL_ClearQueuedAudio(a->device);
    }
    a->enabled = enabled;
}
static void execute_abi1(RecompAudio *a, uint32_t list, uint32_t bytes) {
    a->debug = (RecompAudioSnapshot){.list = list, .bytes = bytes};
    if (!bytes || bytes % 8 || bytes > 131072)
        recomp_fail("invalid audio command-list size", list);
    if (list % 4)
        recomp_fail("audio command list alignment", list);
    uint32_t physical = recomp_ram_address(a->rt, list, bytes);
    memcpy(a->dmem + TASK_DATA_PTR, &physical, 4);
    memcpy(a->dmem + TASK_DATA_SIZE, &bytes, 4);
    a->task_done = false;
    uint64_t start = timer_ns();
    if (a->microcode == RECOMP_AUDIO_NEAD_OOT)
        alist_process_nead_oot(&a->hle);
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
    a->stats.synth_ns += timer_ns() - start;
    if (!a->task_done)
        recomp_fail("audio DSP task did not finish", list);
    a->stats.tasks++;
}
void recomp_audio_execute_abi1(RecompAudio *a, uint32_t list, uint32_t bytes) {
    owner(a);
    if (a->microcode == RECOMP_AUDIO_MUSYX_V1)
        recomp_fail("MusyX requires subframe descriptors, not ABI1 commands", list);
    execute_abi1(a, list, bytes);
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
    uint64_t start = timer_ns();
    musyx_v1_task(&a->hle);
    a->stats.synth_ns += timer_ns() - start;
    if (!a->task_done)
        recomp_fail("MusyX task did not finish", list);
    a->stats.tasks++;
    a->stats.commands += count;
}
static bool microcode_matches(RecompAudio *a, uint32_t address, unsigned bytes, const char *hex) {
    uint8_t *data = malloc(bytes);
    if (!data)
        return false;
    for (unsigned i = 0; i < bytes; i++)
        data[i] = (uint8_t)recomp_load(a->rt, address + i, 1);
    uint8_t digest[32];
    unsigned size = 0;
    bool okay = EVP_Digest(data, bytes, digest, &size, EVP_sha256(), NULL) != 0 && size == 32;
    free(data);
    for (unsigned i = 0; okay && i < 32; i++) {
        unsigned expected;
        if (sscanf(hex + i * 2, "%2x", &expected) != 1 || digest[i] != expected)
            okay = false;
    }
    return okay;
}
bool recomp_audio_submit(RecompAudio *a, const RecompTask *task) {
    owner(a);
    if (task->resumed || task->words[0] != 2 || (task->words[1] & ~RECOMP_TASK_USER_FLAGS))
        return false;
    const RecompAudioTaskProfile *profile = a->task_profile;
    if (!profile)
        return false;
    for (unsigned i = 0; i < 6; i++)
        if (task->words[2 + i] != profile->descriptor[i])
            return false;
    if (!a->microcode_checked) {
        for (unsigned i = 0; i < 3; i++)
            if (!microcode_matches(a, task->words[2 + i * 2], profile->fingerprint_bytes[i], profile->sha256[i]))
                return false;
        a->microcode_checked = true;
    }
    /* The task hook parks the guest in recomp_on_host. Write live RAM before
     * queuing SP completion; the task service delivers its event on polling. */
    if (a->microcode == RECOMP_AUDIO_MUSYX_V1)
        execute_musyx(a, task->words[12], task->words[13]);
    else
        execute_abi1(a, task->words[12], task->words[13]);
    if (!recomp_task_complete(a->rt, task->token, RECOMP_SP_DONE))
        recomp_fail("invalid audio task completion", task->guest_address);
    return true;
}
RecompAudioStats recomp_audio_stats(RecompAudio *a) {
    owner(a);
    RecompAudioStats result = a->stats;
    result.queued_bytes = a->device ? SDL_GetQueuedAudioSize(a->device) : 0;
    if (result.min_queued_bytes == UINT32_MAX)
        result.min_queued_bytes = 0;
    return result;
}
RecompAudioSnapshot recomp_audio_snapshot(const RecompAudio *a) {
    owner((RecompAudio *)a);
    RecompAudioSnapshot out = a->debug;
    uint64_t first = out.command_index - out.history_count;
    for (size_t i = 0; i < out.history_count; i++)
        out.history[i] = a->debug.history[(first + i) % RECOMP_AUDIO_HISTORY];
    return out;
}
