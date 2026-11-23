#pragma once
#include "recomp_audio.h"
#include "rsp.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#ifdef RECOMP_WITH_AUDIO_HLE
#include "hle_internal.h"
#include <setjmp.h>
#endif

struct RecompAudio {
    Runtime *rt;
#ifdef RECOMP_WITH_AUDIO_HLE
    struct hle_t hle;
    _Alignas(16) uint8_t dmem[4096];
    unsigned book_coefficients;
    jmp_buf skip_task;
    bool hle_active, task_done;
#endif
    RecompAudioStats stats;
    RecompAudioSnapshot debug;
    FILE *wav;
    FILE *trace;
    SDL_AudioDeviceID device;
    SDL_threadID owner;
    bool playback, enabled, microcode_checked, sdl_audio;
    RecompAudioMicrocode microcode;
    const RecompAudioTaskProfile *task_profile;
    RecompRspState *rsp;
#ifdef RECOMP_DEBUG
    uint8_t *compare_ram;
    struct AudioSpan {
        uint32_t address, bytes;
        bool pcm;
    } *spans;
    size_t span_count, span_capacity;
    uint32_t interleave_start, interleave_end;
#endif
    unsigned prebuffer_ms;
    unsigned device_period;
};

uint64_t recomp_audio_timer_ns(void);
uint32_t recomp_audio_prepare_list(RecompAudio *, uint32_t list, uint32_t bytes);
void recomp_audio_record_command(RecompAudio *, uint32_t w1, uint32_t w2, uint32_t pc);
#ifdef RECOMP_WITH_AUDIO_HLE
void recomp_audio_execute_hle(RecompAudio *, uint32_t list, uint32_t size);
#ifdef RECOMP_DEBUG
void recomp_audio_compare_hle(RecompAudio *, uint32_t list, uint32_t bytes);
#endif
#endif
