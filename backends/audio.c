#define _POSIX_C_SOURCE 200809L
#include "audio_internal.h"
#include "runtime_debug.h"
#include "timing.h"
#ifdef __EMSCRIPTEN__
#include <emscripten/threading.h>
#include <emscripten/proxying.h>
#endif
#include "hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <inttypes.h>

uint64_t recomp_audio_timer_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t))
        abort();
    return (uint64_t)t.tv_sec * UINT64_C(1000000000) + (uint64_t)t.tv_nsec;
}
static void owner(RecompAudio *a) {
    if (a->owner != SDL_ThreadID())
        recomp_fail("audio accessed outside owner thread", 0);
}
void recomp_audio_record_command(RecompAudio *a, uint32_t w1, uint32_t w2, uint32_t pc) {
    a->debug.history[a->debug.command_index % RECOMP_AUDIO_HISTORY] = (RecompAudioCommand){pc, w1, w2};
    a->debug.command_index++;
    if (a->debug.history_count < RECOMP_AUDIO_HISTORY)
        a->debug.history_count++;
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
static bool open_device_impl(RecompAudio *a, unsigned frequency) {
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
    a->device_period = have.samples;
    a->stats.device_open = true;
    snprintf(a->stats.driver, sizeof(a->stats.driver), "%s", SDL_GetCurrentAudioDriver());
    if (a->trace)
        fprintf(stderr, "Audio trace: driver=%s rate=%d device_period=%u prebuffer_frames=%u\n", a->stats.driver,
                have.freq, have.samples, a->stats.prebuffer_frames);
    return true;
}
#ifdef __EMSCRIPTEN__
struct AudioOpenRequest {
    RecompAudio *audio;
    unsigned frequency;
    bool okay;
};
static void open_browser_device(void *opaque) {
    struct AudioOpenRequest *request = opaque;
    request->okay = open_device_impl(request->audio, request->frequency);
}
#endif
static bool open_device(RecompAudio *a, unsigned frequency) {
    if (a->device)
        return true;
#ifdef __EMSCRIPTEN__
    /* SDL's WebAudio initialization reads main-thread Module state. Keep only
     * device creation on that thread; guest synthesis stays with its owner. */
    struct AudioOpenRequest request = {a, frequency, false};
    em_proxying_queue *queue = em_proxying_queue_create();
    if (!queue)
        return false;
    bool completed = emscripten_proxy_sync(queue, emscripten_main_runtime_thread_id(), open_browser_device, &request);
    em_proxying_queue_destroy(queue);
    return completed && request.okay;
#else
    return open_device_impl(a, frequency);
#endif
}
static void reset_playback(RecompAudio *a) {
    SDL_PauseAudioDevice(a->device, 1);
    a->stats.playback_running = false;
}
static bool queue_playback(RecompAudio *a, const int16_t *pcm, unsigned frames) {
    unsigned queued = SDL_GetQueuedAudioSize(a->device), rate = a->stats.frequency;
    if (a->stats.playback_running) {
        if (queued < a->stats.min_queued_bytes)
            a->stats.min_queued_bytes = queued;
        if (!queued) {
            a->stats.queue_underruns++;
            reset_playback(a);
            /* Repeated stalls need more headroom than the initial reserve.
             * Retain the larger target until output is disabled or destroyed. */
            unsigned ceiling = rate / 5;
            if (ceiling < 3u * a->device_period)
                ceiling = 3u * a->device_period;
            unsigned reserve = a->stats.prebuffer_frames + rate / 25;
            a->stats.prebuffer_frames = reserve < ceiling ? reserve : ceiling;
        }
    }
    /* Bound latency if the device stalls; never accumulate seconds of old sound. */
    unsigned limit = rate;
    if (limit < (a->stats.prebuffer_frames + a->device_period) * 4)
        limit = (a->stats.prebuffer_frames + a->device_period) * 4;
    if (queued > limit) {
        reset_playback(a);
        SDL_ClearQueuedAudio(a->device);
        a->stats.queue_resets++;
    }
    /* Preserve synthesized samples and their original rate at SDL admission.
     * Buffer reserves absorb jitter without changing the waveform or pitch. */
    bool okay = SDL_QueueAudio(a->device, pcm, frames * 4) == 0;
    if (okay) {
        a->stats.playback_frames += frames;
        queued = SDL_GetQueuedAudioSize(a->device);
        if (queued > a->stats.max_queued_bytes)
            a->stats.max_queued_bytes = queued;
        if (!a->stats.playback_running && queued >= a->stats.prebuffer_frames * 4) {
            SDL_PauseAudioDevice(a->device, 0);
            a->stats.playback_running = true;
        }
    }
    return okay;
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
        if (okay)
            okay = queue_playback(a, pcm, bytes / 4);
    }
    a->stats.frames += bytes / 4;
    a->stats.ai_buffers++;
    if (a->trace)
        fprintf(a->trace, "%" PRIu64 ",%u,%u,%u,%" PRIu64 ",%u\n", recomp_audio_timer_ns(), bytes / 4, rate,
                a->device ? SDL_GetQueuedAudioSize(a->device) : 0, a->stats.queue_underruns, a->stats.prebuffer_frames);
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
static void ai_pause(void *opaque, bool paused) {
    RecompAudio *a = opaque;
    owner(a);
    if (a->device)
        SDL_PauseAudioDevice(a->device, paused || !a->stats.playback_running);
}

RecompAudio *recomp_audio_create(Runtime *rt, const RecompAudioConfig *config) {
    if (!rt || !rt->platform || !config || config->prebuffer_ms > 200 ||
        (config->microcode != RECOMP_AUDIO_ABI1 && config->microcode != RECOMP_AUDIO_ABI1_LINEAR &&
         config->microcode != RECOMP_AUDIO_NEAD_OOT && config->microcode != RECOMP_AUDIO_NAUDIO_MP3 &&
         config->microcode != RECOMP_AUDIO_NAUDIO_CBFD && config->microcode != RECOMP_AUDIO_NAUDIO &&
         config->microcode != RECOMP_AUDIO_MUSYX_V1 && config->microcode != RECOMP_AUDIO_NEAD_1080 &&
         config->microcode != RECOMP_AUDIO_NEAD_MM))
        return NULL;
#ifndef RECOMP_WITH_AUDIO_HLE
    if (config->task_profile && !config->task_profile->rsp)
        return NULL;
#endif
    RecompAudio *a = calloc(1, sizeof(*a));
    if (!a)
        return NULL;
    a->rt = rt;
    a->owner = SDL_ThreadID();
    a->playback = config->playback;
    a->enabled = config->output_enabled;
    a->microcode = config->microcode;
    a->task_profile = config->task_profile;
    a->prebuffer_ms = config->prebuffer_ms ? config->prebuffer_ms : 160;
    a->stats.min_queued_bytes = UINT32_MAX;
#ifdef RECOMP_WITH_AUDIO_HLE
    a->hle.dram = rt->rdram;
    a->hle.dmem = a->dmem;
    a->hle.user_defined = a;
#endif
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
        fputs("host_ns,frames,rate,queued_bytes,underruns,prebuffer_frames\n", a->trace);
    }
    RecompAiOutput output = {.start = ai_start, .opaque = a, .pause = ai_pause};
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
    if (a->task_profile && a->task_profile->rsp) {
        const char *mode = getenv("RECOMP_AUDIO_RSP");
#ifdef RECOMP_WITH_AUDIO_HLE
        if (mode && !strcmp(mode, "hle"))
            return a;
#endif
        uint8_t *ram = rt->rdram;
        if (mode && *mode && strcmp(mode, "compiled")) {
#ifdef RECOMP_DEBUG
            if (strcmp(mode, "compare"))
                recomp_fail("RECOMP_AUDIO_RSP must be compiled, hle or compare", 0);
            if (a->microcode != RECOMP_AUDIO_ABI1 && a->microcode != RECOMP_AUDIO_ABI1_LINEAR)
                recomp_fail("audio comparison requires ABI1", 0);
            a->compare_ram = malloc(rt->rdram_size);
            if (!a->compare_ram)
                recomp_fail("audio comparison RAM allocation failed", 0);
            ram = a->compare_ram;
#elif defined(RECOMP_WITH_AUDIO_HLE)
            recomp_fail("RECOMP_AUDIO_RSP must be compiled or hle in Release", 0);
#else
            recomp_fail("this build supports only RECOMP_AUDIO_RSP=compiled", 0);
#endif
        }
        a->rsp = malloc(sizeof(*a->rsp));
        if (!a->rsp)
            recomp_fail("audio RSP allocation failed", 0);
        recomp_rsp_init(a->rsp, ram, rt->rdram_size, a->task_profile->rsp);
    }
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
    if (a->rsp) {
        recomp_rsp_destroy(a->rsp);
        free(a->rsp);
    }
#ifdef RECOMP_DEBUG
    free(a->compare_ram);
    free(a->spans);
#endif
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
        reset_playback(a);
        SDL_ClearQueuedAudio(a->device);
        a->stats.prebuffer_frames = a->stats.frequency * a->prebuffer_ms / 1000;
        if (a->stats.prebuffer_frames < 3u * a->device_period)
            a->stats.prebuffer_frames = 3u * a->device_period;
    }
    a->enabled = enabled;
}
uint32_t recomp_audio_prepare_list(RecompAudio *a, uint32_t list, uint32_t bytes) {
    a->debug = (RecompAudioSnapshot){.list = list, .bytes = bytes};
    if (!bytes || bytes % 8 || bytes > 131072)
        recomp_fail("invalid audio command-list size", list);
    if (list % 4)
        recomp_fail("audio command list alignment", list);
    return recomp_ram_address(a->rt, list, bytes);
}
static void execute_compiled(RecompAudio *a, const RecompTask *task) {
    uint32_t list = task->words[12], bytes = task->words[13];
    uint32_t physical = recomp_audio_prepare_list(a, list, bytes);
#ifdef RECOMP_DEBUG
    if (a->compare_ram)
        memcpy(a->compare_ram, a->rt->rdram, a->rt->rdram_size);
#endif
    uint64_t start = recomp_audio_timer_ns();
    if (!recomp_rsp_start(a->rsp, task->words))
        recomp_fail(a->rsp->error, a->rsp->error_address);
    if (!(a->rsp->cp0[4] & RECOMP_RSP_STATUS_TASKDONE) || a->rsp->rdp_count)
        recomp_fail("invalid compiled audio task completion", task->guest_address);
#ifdef RECOMP_DEBUG
    if (a->compare_ram) {
        recomp_audio_compare_hle(a, list, bytes);
        return;
    }
#endif
    uint64_t elapsed = recomp_audio_timer_ns() - start;
    a->stats.synth_ns += elapsed;
    recomp_timing_add(a->rt, RECOMP_TIMING_AUDIO, elapsed, 1);
    a->stats.tasks++;
    a->stats.commands += bytes / 8;
    for (unsigned i = 0; i < bytes; i += 8) {
        uint32_t pc = physical + i;
        uint32_t w1 = recomp_load32(a->rt, 0x80000000u | pc);
        unsigned opcode = w1 >> 24;
        if (opcode < 32)
            a->stats.opcodes[opcode]++;
        recomp_audio_record_command(a, w1, recomp_load32(a->rt, 0x80000000u | (pc + 4)), pc);
    }
}
static bool microcode_matches(RecompAudio *a, uint32_t address, unsigned bytes, const char *hex) {
    uint8_t *data = malloc(bytes);
    if (!data)
        return false;
    for (unsigned i = 0; i < bytes; i++)
        data[i] = (uint8_t)recomp_load(a->rt, address + i, 1);
    uint8_t digest[32];
    bool okay = recomp_sha256(data, bytes, digest);
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
#ifdef RECOMP_WITH_AUDIO_HLE
    if (!a->rsp)
        recomp_audio_execute_hle(a, task->words[12], task->words[13]);
    else
#endif
        execute_compiled(a, task);
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
