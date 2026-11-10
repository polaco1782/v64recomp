# Game layout conventions

For camera/FOV extensions, follow [the FOV investigation guide](docs/field-of-view.md)
to trace projection, CPU visibility and scope against the exact ROM.

When adding or extending a game, read [SKILL.md](SKILL.md) and follow the layout
in [docs/adding-a-rom.md](docs/adding-a-rom.md). Apply the same filenames and
generation interface to every game:

```text
games/<game>/
  CMakeLists.txt
  README.md
  profile.toml
  generate.py
  main.c                  # single entry point and game lifecycle, both builds
  audio.h                 # when needed
  *.h, *.c, *.cpp, *.py    # headers, shared adapters and generation helpers
  *.json                  # checked inventories and game metadata, when needed
  *.txt                   # maintained replay inputs, when needed
```

- Keep one complete `profile.toml` per game. Put ROM identity, mappings,
  bootstrap settings, discovery policy, tables, hooks and boundaries there.
  Do not introduce `full.toml`, `startup.toml`, `startup-base.toml`,
  `services.toml`, `dispatches.toml`, or other game profile inheritance chains.
- Declare checked tables in `[discovery]`: `jump_tables` for local switches,
  `function_tables` for indirect calls, and `transfers` for other checked target
  sets or returns. Keep overlay mappings and game-specific table configuration
  in this same profile; do not split them into JSON configuration sidecars.
  Preparation helpers resolve compressed data from these declarations. JSON
  code/entry inventories remain separate ROM-bound evidence files.
- Keep all game code, headers, generation helpers and maintained replay inputs
  directly in `games/<game>/`. Do not create `debug/` or `release/` source folders,
  `app.c`, separate mode entry points, or a `run.c`/`run.h` launcher layer.
- Put the single `int main(int argc, char **argv)` and game lifecycle in `main.c`.
  Organize it as includes, game state, static helpers/native hooks, then `main`:
  parse options, initialize runtime/ROM/services/backends, boot, run the frame
  loop, and tear down. Share this control flow between both configurations.
  Use small `#ifdef RECOMP_DEBUG` blocks only for genuinely game-specific
  diagnostics; do not duplicate a whole main function or loop under `#else`.
  Keep distinct input, graphics, pager or codec adapters in root-level files
  only when they have an independent responsibility. Make private helpers static.
- Use `recomp_game_options`, `recomp_game_rom`, `recomp_game_input`,
  `recomp_game_present` and `recomp_game_audio_create` from `game.h`.
  The shared game layer owns Debug options, controller replay, captures and audio
  recording; core dispatch owns call tracing. Debug input adapters use
  `recomp_game_replaying()` to disable live mouse overrides during replay. Do not
  add per-game argument translation, controller replay parsers, report writers
  or standalone validation harnesses.
  Release keeps ROM/runtime correctness checks and shared crash reporting.
- Put `generate.py` and preparation/configuration helpers in the parent folder.
  Export `write_config(profile, metadata, output, startup)` and use the optional
  hooks supported by `tools/generate_game.py`. Emit the main boot configuration
  as `game_config.h` in the generated output directory; keep game-specific
  generation behavior in the game module rather than adding game-ID branches
  to the driver.
- Keep Python helpers only when generation imports them or a documented,
  maintained command needs them. A project-file listing or cache glob does not
  establish use. Remove one-off analyzers/importers after their reviewed results
  are recorded in the profile or inventory. Keep JSON only when a generator,
  profile or maintained tool reads it; do not retain scratch reports or obsolete
  replay JSON. Maintained replays use the runtime's text format directly.
- Use `RECOMP_GAME_SOURCES` for additional root-level translation units.
  Include `cmake/Game.cmake`; it always selects `main.c` and defines
  `RECOMP_DEBUG` for Debug. Do not add per-mode source lists or list `main.c`
  again. Build output directories and CMake presets still separate the modes.
- Keep generated sources in the selected `build/` directory and private ROMs
  and saves in `local/`. Put cross-game services in `runtime/`, `include/`,
  `backends/`, or `tools/` only when their behavior is shared between games.
- Reuse the shared C11 GLES2 renderer and compiled RSP worker targets. Keep
  verified bootstrap/program images in each game's profile and generated C in
  its selected build directory. Do not add display-list HLE or per-game renderers.
- Use `recomp_fail(message, address)` or
  `recomp_game_require(condition, message, address)` for fatal failures in every
  game and both configurations. Load the ROM through `recomp_game_rom()`, which
  installs the shared reporter. Keep crash serialization, stack/register/RAM
  dumps and recent-call history in the core; do not add per-game failure
  callbacks, crash-report headers, JSON writers or private assertion handlers.

Before handing off a new game, check that its file layout, CMake source lists,
tool paths and documentation follow these conventions. Remove unused scripts,
temporary checks/tests and superseded helpers after validating the change.
Run `tools/format.sh`, regenerate the browsable projects with
`python3 tools/msvc_solution.py`, and verify both build configurations.
Preserve this structure even when using an older port or external project as a reference.
