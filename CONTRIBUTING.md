# Contributing to UltiMedia

Thanks for your interest in improving UltiMedia! This core is a small, modular C codebase, so most contributions are approachable. This guide covers how to set up a dev environment, run the tests, build the core, and match the project's code style.

By participating, you agree to abide by our [Code of Conduct](CODE_OF_CONDUCT.md).

## Ways to contribute

- **Report bugs** or **request features** via [GitHub Issues](https://github.com/webhead2oo9/UltiMedia/issues) — the issue forms will guide you.
- **Submit pull requests** for fixes and improvements. For larger changes, opening an issue first to discuss is appreciated.
- **Improve docs** — README clarifications and edits to this guide count too.

## Project layout

All source lives in `src/`, split by responsibility:

| File | Responsibility |
| ---- | -------------- |
| `core.c` | LibRetro callbacks, playlist loading, runtime state, shuffle, save/load state |
| `audio.c` | Audio decoding, resampling, decoder snapshot restore |
| `audio_codecs.c` | Third-party audio decoder implementations (`dr_*`, `stb_vorbis`) |
| `video.c` | Framebuffer and display |
| `visualizer.c` | Audio visualizations (Bars, VU Meter, Dots, Line) |
| `metadata.c` | Track metadata parsing and album art lookup/loading |
| `image_codecs.c` | Third-party image decoder implementation (`stb_image`) |
| `config.c` | LibRetro core options |
| `layout.c` | Responsive UI layout computation |

Tests live in `tests/`, and the Windows contributor helpers in `scripts/`.

## Prerequisites

- **Python 3** — drives the test harness and fixture generation.
- **A C99 compiler:**
  - macOS / Linux: the system `cc` (clang or gcc) works out of the box.
  - Windows: GCC from **MSYS2 UCRT64** — the project targets this toolchain, matching CI.

### Third-party dependencies

The core depends on a few single-file headers that are **not vendored** in the repo — they are fetched into a gitignored `deps/` folder. Pinned sources:

| File | Source |
| ---- | ------ |
| `libretro.h` | RetroArch `v1.7.5` |
| `dr_mp3.h`, `dr_wav.h`, `dr_flac.h` | [mackron/dr_libs](https://github.com/mackron/dr_libs) (`master`) |
| `stb_image.h`, `stb_vorbis.c` | [nothings/stb](https://github.com/nothings/stb) (`master`) |

## Setup, test, and build

### macOS / Linux

There is no setup script for these platforms, so fetch the dependencies once, then run the harness:

```bash
mkdir -p deps
curl -sL -o deps/libretro.h  https://raw.githubusercontent.com/libretro/RetroArch/v1.7.5/libretro-common/include/libretro.h
curl -sL -o deps/dr_mp3.h    https://raw.githubusercontent.com/mackron/dr_libs/master/dr_mp3.h
curl -sL -o deps/dr_wav.h    https://raw.githubusercontent.com/mackron/dr_libs/master/dr_wav.h
curl -sL -o deps/dr_flac.h   https://raw.githubusercontent.com/mackron/dr_libs/master/dr_flac.h
curl -sL -o deps/stb_image.h https://raw.githubusercontent.com/nothings/stb/master/stb_image.h
curl -sL -o deps/stb_vorbis.c https://raw.githubusercontent.com/nothings/stb/master/stb_vorbis.c

python3 tests/run_tests.py
```

`run_tests.py` generates temporary WAV and `.m3u` fixtures, compiles the native test harness, and exercises playback, navigation, shuffle, reset, and save-state behavior. It uses `cc` by default; override with `CC=/path/to/compiler`.

> The distributable artifact is a Windows DLL, so the build command below targets MSYS2. On macOS/Linux the test harness is the primary local validation path — it compiles and links every `src/` module, so a clean run means the code at least builds across the codebase.

### Windows (MSYS2 UCRT64)

The repo ships PowerShell wrappers that assume MSYS2 is installed at `C:\msys64`. Run them from PowerShell at the repo root:

```powershell
.\scripts\setup-windows.ps1   # install toolchain packages + fetch deps
.\scripts\test-windows.ps1    # fetch deps (if needed) + run the harness
.\scripts\build-windows.ps1   # fetch deps (if needed) + build the DLL
```

To build the core manually inside a UCRT64 shell (with `deps/` already populated):

```bash
gcc -shared -O2 -I./deps -I./src -o music_playlist_libretro.dll \
  src/core.c src/audio.c src/audio_codecs.c src/video.c src/visualizer.c \
  src/metadata.c src/image_codecs.c src/config.c src/layout.c -lm
```

## Code style

- **C99**, 4-space indentation.
- Static globals hold state (this is a LibRetro core, not a reusable library).
- Inline comments for non-obvious logic.
- Keep functions focused and under ~50 lines where practical.
- Use `uint16_t` for RGB565 framebuffer operations and `int16_t` for audio sample buffers.
- Route diagnostics through `core_log(level, ...)` (see `src/core_log.h`) rather than raw `fprintf(stderr, ...)`, so the frontend's log level controls verbosity.

Key constants:

```c
#define OUT_RATE 48000          // Fixed output sample rate
#define SAMPLES_PER_FRAME 800   // Audio samples per video frame
```

### Constraints to respect

- The display is fixed at **320x240, RGB565** — keep rendering within those bounds.
- Keep memory usage minimal (embedded/emulator context).
- **New UI elements usually need both responsive layout handling and a non-responsive fallback placement.** Put audio/visual logic in the matching `src/` module.

## Testing your changes

1. Run the native harness first: `python3 tests/run_tests.py`.
2. For changes that affect runtime UX (rendering, layout, controls), also walk the relevant items in [`tests/SMOKE_CHECKLIST.md`](tests/SMOKE_CHECKLIST.md) in a real frontend.

CI runs the harness on Windows, Ubuntu, and macOS, builds the Windows DLL, and runs `clang --analyze` (non-blocking) on every push and PR — so please make sure the harness passes locally before opening a PR.

## Pull request process

1. Fork and create a topic branch.
2. Make your change, following the style above.
3. Ensure `python3 tests/run_tests.py` passes.
4. Open a PR using the template; describe the change, link any related issue, and note how you tested it.
5. Keep CI green — reviewers will look there first.

> Heads up: `CLAUDE.md` in the repo root holds guidelines for AI-assisted contributions and mirrors much of this guide. If you change the build, test, or style conventions, update both.
