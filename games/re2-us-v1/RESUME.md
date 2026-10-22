# RE2 technical bring-up

The current prepared analysis image has SHA-1
`39cad0b1a053431fc281dbf0308288a8287b910c`. Regenerate metadata before using
inspection tools; older crash RAM remains useful, but older metadata can name
different sections or analysis bytes.

Boot, main and overlays 3/2/4/5 are described in the single profile. Overlays
2/4/5 share `0x8017b410` and use `overlay_group` with section-qualified roots
and tables. Their compressed bytes are cartridge offsets; the appended
analysis offsets are never DMA addresses. Loader activation requires the
complete decoded image hash after the original inflater returns.

The opening movie and title menu render. A completed Debug run recorded
1,800 presentations, 3,006 graphics tasks, 734,208 audio frames and audio peak
24,273; its WAV contains nonzero stereo PCM. Gameplay and audible quality
remain unaccepted. The deterministic `debug/new-game.txt` replay gets beyond
character selection and reaches overlay 5. Before that mapping was added,
the loader stopped at `0x8017b410` with a complete 0x5200-byte decoded image.
The captured image matched overlay 5 exactly. Its callback `0x8017b4a4` comes
from main module-table record 4 at `0x8010b300`.

Retained private evidence:

- `build/re2-us-v1-debug/movie-validation.log`, `movie.wav`, and
  `debug-output/re2-us-v1/boot-report.json` describe the completed movie run.
- `local/crashes/re2-us-v1/crash-2779708-886167805170423.json` and its
  `.rdram.bin` contain the decoded overlay 5 trigger before mapping it.
- `build/re2-us-v1-sdk/overlay5.json` records the full ROM/RAM comparison.

Reproduce the comparison from the repository root:

```sh
.venv/bin/python games/re2-us-v1/debug/inspect_overlay.py --index 5 \
  --ram local/crashes/re2-us-v1/crash-2779708-886167805170423.json.rdram.bin
```

Continue with a bounded Debug replay using `--input
games/re2-us-v1/debug/new-game.txt`, inspect actual captured presentations,
and investigate the next reached boundary. Keep RE2 diagnostics and replay
inputs in this source directory; private reports, binaries and ROM/RAM data
belong under ignored `build/` and `local/`.
