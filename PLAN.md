# Native ROM conversion: SM64, GoldenEye, Shadow Man, Quake 64, Duke Nukem 64 and Nightmare Creatures

Game profiles, applications, launchers and game-specific diagnostics live under `games/`.
The original emulator is separate. SM64 is the accepted gameplay baseline;
GoldenEye is the second ROM target.
Shadow Man US is the third target, isolated under `games/shadowman-us/`.
Quake 64 US is the fourth target, isolated under `games/quake-us/`.
Duke Nukem 64 US is the fifth target, isolated under `games/duke-us/`.
Nightmare Creatures US is the sixth target, isolated under `games/nightmare-us/`.
It reaches title, character selection and playable Chelsea with native graphics,
audio and controller input. Full-game validation and cross-game regressions are
deferred until after this initial bring-up.

For future game additions, ordinary OS/library calls belong in the shared
native hook catalog. Reuse known entries; add verified SDK variants and missing
shared services there. Reserve game-profile hooks for game-specific behavior.
See [the contribution rules](docs/native-hooks.md).

The ROM-only inventory classifies all 1,385,456 bytes in the three mapped sections:
1,203,388 bytes of ordinary CPU text, 21,860 bytes of native OS/library implementation,
8,960 bytes of embedded RSP programs and 151,248 bytes of data. All ordinary
CPU text, including unreachable instructions, converts to C. Its 4,373 function
entry points and 52 native OS/library hooks compile successfully. The generated CPU code
contains no fetch/decode interpreter and no unsupported game-function boundaries.

The full profile uses registered function addresses for indirect calls. Local
computed jumps still need checked table descriptors. ROM entry/code-range hints
are mandatory metadata, not recovered high-level game structures. A matching
ELF/map was used to cross-check the SM64 inventory during development; the current
build, generation and app do not consume reference artifacts.

The shared catalog automatically recognizes verified OS and memory/string
routines; game TOML overrides handle unique behavior. See [native hooks](docs/native-hooks.md).
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

Shadow Man milestones (see [setup and status](games/shadowman-us/README.md)):

1. Completed: import and pin the supplied US ROM, trace original load mappings,
   validate both BSS clears and initial thread creation in translated C.
2. Completed: FR=1 FPU/thread layouts, native fault delivery to the original
   page-cache workers, checked callback roots and switch tables, native OS
   boundaries, and the first original graphics task. ASan/UBSan checks pass.
3. In progress: RT64/SDL rendering and audio, live controller reads and original
   menu progression. The Controller Pak prompt renders with matching SP/DP
   callbacks and zero audio underruns in a bounded desktop run. Controller Pak
   storage and broader title/gameplay validation remain to be completed.
4. Complete the CPU inventory and validate gameplay, progression and saves.

Quake 64 milestones (see [setup and status](games/quake-us/README.md)):

1. Completed: pin the supplied US ROM and validate original BSS clearing and
   first-thread creation in translated C. UBSan and ASan/UBSan bootstrap checks pass.
2. Completed: checked callback roots and switch tables, shared SDK recognition
   and native startup through the first original ABI1 audio task. The partial
   conversion has 1,404 translated functions and 55 catalog bindings, with no
   game-profile OS overrides.
3. Completed bounded checks: RT64/SDL startup, title/menu progression and first
   level gameplay with forward movement, turning and shotgun firing. Original
   split graphics lists complete with real SP receipts and DP only on full sync.
   Desktop pixels and nonzero PCM are captured. SM64's rendered regression passes.
4. Complete the CPU inventory and validate prolonged gameplay, progression and
   Controller Pak saves. One audio underrun occurred in the final gameplay check;
   audio continuity remains under validation.

Duke Nukem 64 milestones (see [setup and status](games/duke-us/README.md)):

1. Completed: pin the US ROM, map its initialized image, and validate original
   BSS clearing, first-thread creation and the first ABI1 task with UBSan.
2. Completed: 743 translated functions, 55 shared catalog bindings, checked
   callback entries and local switch tables. The game uses 8 MiB backing for
   original sprite-sort reads above the base RAM bank; game code stays translated.
3. Completed bounded check: optimized RT64/SDL startup, menus and the first
   level with movement, turning and pistol firing. The 2,700-presentation desktop
   run captured visible gameplay and nonzero PCM with matching graphics callbacks
   and zero audio queue underruns/resets.
4. Next: regression checks, broader gameplay/progression, Controller Pak saves
   and a complete CPU inventory. Other games were not retested for this addition.
