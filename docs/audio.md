# Native audio

Each game runs its translated sound code and reads tables, sequences and
sample assets from its ROM. `backends/audio.c` executes ABI1 command operations
using the pinned native DSP subset in `third_party/rsp-hle/`, sourced from
[Mupen64Plus RSP HLE](https://github.com/mupen64plus/mupen64plus-rsp-hle).
ADPCM, codebooks, envelopes, four-tap resampling, mixing, reverb-buffer operations,
filtering and stereo interleave write PCM into guest RAM. One SP completion is
posted after synthesis. No CPU or RSP instruction interpreter is linked.

Games provide pinned task addresses and boot/text/data microcode hashes through
`games/<game>/audio.h`. The shared adapter validates these fingerprints and each
command's RAM/DMEM ranges, alignment and supported flags. Unknown microcodes,
commands and unsupported resampling flags fail explicitly. Revision, license and
local changes are in [third_party/rsp-hle/README.md](../third_party/rsp-hle/README.md).

`runtime/platform.c` owns the two-entry AI FIFO and exact clock/divisor timing.
Length reports the active buffer; a full FIFO rejects admission with -1. Queued
transfers start at the previous buffer's exact end, retaining fractional timing.
Consumed buffers post AI events, with bounded transport retries and the SDK's
nonblocking behavior for a full guest queue. Host sound-device progress does not
determine guest completion.

`recomp_ai_set_output` attaches an owner-thread PCM sink. It copies a buffer when
it becomes active, including promotion of a queued successor. SDL receives signed
16-bit stereo samples; optional WAV capture stores canonical little-endian PCM.
Synthesis does not play samples directly. A stalled device's queue is bounded to
avoid seconds of stale sound. Rate changes after output starts remain unsupported.
Playback starts after an 80 ms reserve (at least three SDL device periods) has
accumulated. An empty running queue counts as an underrun and pauses the device
until the reserve is refilled. Disabling output clears and pauses playback.
`audio_results.json` records `queue_underruns`, `queue_resets` (excess queue),
`min_queued_bytes`, `max_queued_bytes` and `prebuffer_frames`; the minimum measures
the running queue before PCM admission, excluding initial buffering.

```sh
.venv/bin/python games/sm64-us/run_sm64.py --render
.venv/bin/python games/sm64-us/run_sm64.py --render --mute
.venv/bin/python games/sm64-us/run_sm64.py --render --capture
.venv/bin/python games/sm64-us/run_sm64.py --playback --mode jump
```

Interactive output uses wall time from startup. Deterministic gameplay checks
synthesize during accelerated startup and enable capture/output when Mario becomes
controllable. `--mute` preserves synthesis and timing. Window close finalizes an
enabled WAV capture and audio statistics.
The rendered app sleeps only for the portion of each 60 Hz interval remaining
after guest work and presentation, keeping audio service on schedule.

Captured ROM scenarios can verify stereo output, bounded clipping and waveform
changes following input. Desktop playback remains the check for audible quality
during broader gameplay.
