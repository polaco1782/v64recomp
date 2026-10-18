# Game layout conventions

When adding or extending a game, read [SKILL.md](SKILL.md) and follow the layout
in [docs/adding-a-rom.md](docs/adding-a-rom.md). Apply the same filenames and
generation interface to every game:

```text
games/<game>/
  CMakeLists.txt
  README.md
  profile.toml
  generate.py
  audio.h                 # when needed
  *.h, *.c, *.cpp, *.py    # headers, shared adapters and generation helpers
  *.json                  # checked inventories and game metadata, when needed
  debug/
    app.c
    ...                   # Debug-only sources, replay inputs and diagnostics
  release/
    app.c
    ...                   # Release-only sources
```

- Keep one complete `profile.toml` per game. Put ROM identity, mappings,
  bootstrap settings, discovery policy, tables, hooks and boundaries there.
  Do not introduce `full.toml`, `startup.toml`, `startup-base.toml`,
  `services.toml`, `dispatches.toml`, or other game profile inheritance chains.
- Keep all game headers and shared game code in `games/<game>/`. Both modes
  use the parent folder; neither mode may depend on the other mode's folder.
  Keep independent `debug/app.c` and `release/app.c` entry points.
- Put `generate.py` and preparation/configuration helpers in the parent folder.
  Export `write_config(profile, metadata, output, startup)` and use the optional
  hooks supported by `tools/generate_game.py`; keep game-specific generation
  behavior in the game module rather than adding game-ID branches to the driver.
- Use `RECOMP_GAME_SOURCES` for shared parent-folder translation units and
  `RECOMP_GAME_DEBUG_SOURCES` / `RECOMP_GAME_RELEASE_SOURCES` for additional
  configuration-specific sources. Include `cmake/Game.cmake`; it selects `app.c`.
- Keep generated sources in the selected `build/` directory and private ROMs
  and saves in `local/`. Put cross-game services in `runtime/`, `include/`,
  `backends/`, or `tools/` only when their behavior is shared between games.
- Use `recomp_fail(message, address)` or
  `recomp_game_require(condition, message, address)` for fatal failures in every
  game and both configurations. Load the ROM through `recomp_game_rom()`, which
  installs the shared reporter. Keep crash serialization, stack/register/RAM
  dumps and recent-call history in the core; do not add per-game failure
  callbacks, crash-report headers, JSON writers or private assertion handlers.

Before handing off a new game, check that its file layout, CMake source lists,
tool paths and documentation follow these conventions. Preserve this structure
even when using an older port or external project as a reference.
