# Native audio

Each game runs its translated sound code and reads tables, sequences and
sample assets from its ROM. `backends/audio.c` executes ABI1 and Zelda NEAD command operations
using the pinned native DSP subset in `third_party/rsp-hle/`, sourced from
[Mupen64Plus RSP HLE](https://github.com/mupen64plus/mupen64plus-rsp-hle).
ADPCM, codebooks, envelopes, four-tap resampling, mixing, reverb-buffer operations,
filtering and stereo interleave write PCM into guest RAM. One SP completion is
posted after synthesis. DSP tasks run on the persistent `n64-audio` pthread using
a private RAM snapshot. The host merges task-modified bytes before publishing
the SP event; unrelated guest writes are retained, and conflicting writes fail
explicitly. No CPU or RSP instruction interpreter is linked.

Zelda FILTER setup copies coefficients into an RSP-local cache. Filter execution
updates that cache and the 32-byte guest filter state, preserving the CPU-owned
source table. This permits the game to prepare the following task's coefficients
while synthesis runs. Source coefficients remain CPU-owned across filter execution.

Games provide pinned task addresses and boot/text/data microcode hashes through
`games/<game>/release/audio.h`. The shared adapter validates these fingerprints and each
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
./build/sm64-us-release/bin/sm64-us
./build/sm64-us-release/bin/sm64-us --mute
./build/sm64-us-debug/bin/sm64-us --frames 180 --mode jump
./build/oot-us-v12-debug/bin/oot-us-v12 --audio-trace /tmp/oot-audio.csv
```

Interactive output uses wall time from startup. Deterministic gameplay checks
synthesize during accelerated startup and enable capture/output when Mario becomes
controllable. `--mute` preserves synthesis and timing. Window close finalizes an
enabled WAV capture and audio statistics.
Rendered apps use absolute presentation deadlines. Ocarina of Time additionally
runs bounded scheduler batches during the remaining interval, sleeping at most
1 ms between batches. Its audio manager discards duplicate retrace messages;
servicing VI/AI and worker completions during pacing prevents that coalescing
from starving playback. Its debug executable supports `--audio-trace PATH` for a CSV of
host timestamps, admitted frames, sample rate, SDL queue bytes and underruns.

Captured ROM scenarios can verify stereo output, bounded clipping and waveform
changes following input. Desktop playback remains the check for audible quality
during broader gameplay.
