# Native audio

Each game that submits RSP audio tasks runs its cartridge's bootstrap and synthesis
microcode as generated C11 in Debug and Release, with either renderer. The game
profile's `[audio.rsp].images` identifies the executable bytes by SHA-256,
including IMEM overlays. A game can supply a decompressed or relocated source
through its generator's `rsp_images` hook. Direct PCM producers use the AI
output path independently of the RSP.

The audio adapter checks the task descriptor and boot, text and data fingerprints,
translates guest addresses for SP DMA, and runs the compiled program on live
RDRAM while the guest is parked. IMEM loads bind only matching instructions.
Some bootstraps DMA trailing data along with their code. The profile's
`dma_size` describes that transfer, and executing an untranslated tail faults.
ADPCM histories, filter and resampler state and stereo PCM are written by the
microcode before the SP completion event is queued.

Most audio tasks supply a list of eight-byte commands. Resident Evil 2 and
The World Is Not Enough use MusyX v1. Their task size counts 0xa10-byte
subframe descriptors. Inspect a captured MusyX task with
`tools/analyze.py rsp-task REPORT --list-scale 0xa10 --structured`.

## Cartridge task replay

For each of the 22 games with RSP audio, tasks 0, 32 and 128 were captured at
submission with their descriptor and starting RDRAM. In nine games those three
tasks left RAM unchanged, so a later task that wrote different bytes was also
captured. All 75 tasks produced byte-identical final RDRAM in the compiled
program and in rsp-cxd4 with audio forwarding disabled. This includes PCM,
decoder history and every other byte the microcode wrote. The former audio HLE
matched 46 of the same inputs and differed on 29.

Separate directed tests exercised the MP3 overlay in Conker and Perfect Dark,
including its return to the main audio program. Both matched the reference on
all output bytes.
[Reference interpreter revision](https://github.com/mupen64plus/mupen64plus-rsp-cxd4/tree/2a4a30094fdb9f593157cfec978c6560212752fd)

## SM64 US compiled audio

The bootstrap DMAs 0xf80 bytes, including 0xe20 bytes of executable ASP text.
The task itself advertises 0x800 bytes.

In one SM64 US startup capture, task 11 differed from HLE on 884 of 1,120 PCM
samples (maximum delta 64) and matched all 32 decoder-history bytes. Task 100
differed on 1,053 of 1,056 samples (maximum delta 9,988) and matched all 64
history bytes. Replaying the same descriptors and initial RAM for tasks 11, 12
and 100 through an independent instruction-level RSP interpreter produced the
same final 4 MiB RAM image as compiled ASP for each task. The offline reference
used all-zero plugin configuration with audio forwarding disabled.
[Reference interpreter revision](https://github.com/mupen64plus/mupen64plus-rsp-cxd4/tree/2a4a30094fdb9f593157cfec978c6560212752fd)

## GoldenEye US compiled audio

The original bootstrap and the linear-envelope ASP reside in the verified
RareZip startup image. The audio task declares zero text bytes. Its bootstrap
loads 0xf80 bytes at IMEM 0x80, including 0xdc0 bytes of executable text.
The trailing game data has no compiled instruction entries. The task also loads
0x800 bytes of DMEM data, of which the 0x2c0-byte initialized prefix is fingerprinted.

One 500-frame startup run compared 248 tasks. All 43,008 ADPCM history bytes
matched HLE. Of 360,640 signed PCM samples, 200,724 differed. In task 150,
all 1,440 samples differed, with a maximum absolute delta of 11,280, and all
384 history bytes matched. Replaying the same descriptors and starting RAM for
tasks 90, 120 and 150 through rsp-cxd4 produced byte-identical final 4 MiB RAM
images to the compiled path. Task 90 was silent. Tasks 120 and 150 exercised
active voices. [Reference interpreter revision](https://github.com/mupen64plus/mupen64plus-rsp-cxd4/tree/2a4a30094fdb9f593157cfec978c6560212752fd)

## DMA, synthesis and playback

The native PI manager services accepted cartridge DMA submissions on the owner
thread before returning to guest code. Deferring those transfers until a later
scheduler poll can let an audio task read stale sample data; Quake's streamed
ADPCM samples exposed this ordering bug. DMA completion messages are posted after
the copy, before the submitting guest resumes.

`runtime/platform.c` owns the two-entry AI FIFO and exact clock/divisor timing.
Length reports the active buffer; a full FIFO rejects admission with -1. Queued
transfers start at the previous buffer's exact end, retaining fractional timing.
Consumed buffers post AI events, with bounded transport retries and the SDK's
nonblocking behavior for a full guest queue. Host sound-device progress does not
determine guest completion.

Graphics receipts are published after RT64's display-list interpreter has
finished guest RAM readbacks and copied the inputs needed by its render queues.
The adapter does not wait for GPU presentation before releasing the shared RSP
for audio synthesis. Presentation pacing stays on RT64's queues; texture reload
and shutdown retain their explicit GPU barriers.

`recomp_ai_set_output` attaches an owner-thread PCM sink. It copies a buffer when
it becomes active, including promotion of a queued successor. SDL receives signed
16-bit stereo samples; optional WAV capture stores canonical little-endian PCM.
Synthesis does not play samples directly. A stalled device's queue is bounded to
avoid seconds of stale sound. Rate changes after output starts remain unsupported.
Playback starts after a 160 ms reserve (at least three SDL device periods) has
accumulated; a game's `prebuffer_ms` can select an initial reserve up to 200 ms.
The playback queue receives the original synthesized PCM at the game's sample
rate, preserving sample values, frame counts and pitch. An empty running queue
counts as an underrun and pauses playback until the reserve is refilled; each
underrun raises the reserve by 40 ms, up to 200 ms (or three device periods if larger).
Disabling output clears and pauses playback and restores the initial reserve.
Stalls longer than the available reserve, or repeated stalls that deplete it,
can still interrupt playback. Guest AI completion timing and WAV capture remain
independent of device playback buffering.
Shared audio statistics record `queue_underruns`, `queue_resets` (excess queue),
`min_queued_bytes`, `max_queued_bytes` and `prebuffer_frames`; the minimum measures
the running queue before PCM admission, excluding initial buffering.

```sh
./build/sm64-us-release/bin/sm64-us
./build/sm64-us-release/bin/sm64-us --mute
./build/sm64-us-debug/bin/sm64-us --frames 180 --wav /tmp/sm64-audio.wav
./build/oot-us-v12-debug/bin/oot-us-v12 --audio-trace /tmp/oot-audio.csv
# Opt-in timing diagnostics also work in Release and packaged builds:
RECOMP_AUDIO_TRACE=/tmp/oot-audio.csv ./build/oot-us-v12-release/bin/oot-us-v12
```

Both configurations use the same wall-clock startup and frame loop. Debug
records audio only when `--wav` is supplied. `--mute` preserves synthesis and timing. Window close finalizes an
enabled WAV capture and audio statistics.
Rendered apps use absolute presentation deadlines. Ocarina of Time additionally
runs bounded scheduler batches during the remaining interval, sleeping at most
1 ms between batches. Its audio manager discards duplicate retrace messages;
servicing VI/AI and task completions during pacing prevents that coalescing
from starving playback. Its debug executable supports `--audio-trace PATH` for a CSV of
host timestamps, admitted frames, sample rate, SDL queue bytes, underruns
and the current reserve target.
`RECOMP_AUDIO_TRACE=PATH` enables the same CSV for any build whose audio config
does not specify a trace path. When tracing is enabled, stderr also records the
SDL audio driver, sample rate, device period, prebuffer size and final buffer/
underrun/reset counts. Without an explicit trace request, playback stays quiet.

Captured ROM scenarios can verify stereo output, bounded clipping and waveform
changes following input. Desktop playback remains the check for audible quality
during broader gameplay.
