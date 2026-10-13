# Native ROM conversion: SM64 and GoldenEye

Game profiles, applications, launchers and game-specific diagnostics live under `games/`.
The original emulator is separate. SM64 is the accepted gameplay baseline;
GoldenEye is the second ROM target.

The ROM-only inventory classifies all 1,385,456 bytes in the three mapped sections:
1,204,464 bytes of ordinary CPU text, 20,784 bytes of native OS implementation,
8,960 bytes of embedded RSP programs and 151,248 bytes of data. All ordinary
CPU text, including unreachable instructions, converts to C. Its 4,378 function
entry points and 45 native OS hooks compile successfully. The generated CPU code
contains no fetch/decode interpreter and no unsupported game-function boundaries.

The full profile uses registered function addresses for indirect calls. Local
computed jumps still need checked table descriptors. ROM entry/code-range hints
are mandatory metadata, not recovered high-level game structures. A matching
ELF/map was used to cross-check the SM64 inventory during development; the current
build, generation and app do not consume reference artifacts.

Native OS services cover scheduling, queues, clocks, VI/PI/AI/SI, controller input,
EEPROM and global page aliases. RT64 executes graphics tasks; the ABI1 DSP backend
writes guest PCM and AI activation sends it to SDL. Existing basic gameplay checks
cover Castle Grounds and the castle lobby door warp, with ROM asset replacement,
controller response, audio and isolated saves.

The user has completed SM64's gameplay milestones and reports that the game works
well. Preserve this working baseline while reusing the decoder, emitter and native
services for GoldenEye. Translation coverage and runtime acceptance stay separate.

GoldenEye milestones (see [setup and current status](games/goldeneye-us/README.md)):

1. Completed: import/fingerprint the supplied ROM, audit its load mappings and
   execute bounded native startup. Original BSS clearing, inflater relocation and
   decoded data match; the original main thread is created and stopped before
   gameplay. Normal and ASan/UBSan startup checks pass.
2. Completed: native game-code demand paging, startup OS hooks, checked callback
   entries and local switch tables. Original startup submits audio and GoldenEye
   graphics tasks; RT64 displays the copyright screen and animated Nintendo logo.
   Desktop pixel captures verify visible output. This is a partial CPU inventory.
3. Progress through the remaining title/menu sequence and enter a mission.
   Validate live controls, persistent EEPROM, audible output and video mode changes.
   Dam now loads into its intro and playable view. SDL playback, persisted saves,
   controller replays and live `--frames 0` work; `--cheat-menu` unlocks all Solo
   missions and 23 regular options for the session. Sky half-packet/trapezoid conversion and an
   optimized CPU build address rendering and performance regressions. Larger
   views, audio continuity and prolonged play remain under validation.
4. Complete/fingerprint the CPU inventory, resolve runtime gaps and validate
   mission progression, saves and prolonged play.

GoldenEye uses `0x70000000` library aliases, a relocated inflater and `0x7f000000`
demand-paged ROM text. Provide native page-load/eviction and hardware behavior;
privileged exception/TLB instructions do not execute in a CPU interpreter.
