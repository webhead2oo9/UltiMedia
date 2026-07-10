# UltiMedia — Claude Code Guidelines

Guidance for working on UltiMedia with Claude Code. This is the agent-facing companion to [`CONTRIBUTING.md`](CONTRIBUTING.md) (human-facing) — the two overlap, so **if you change a build, test, or style convention, update both.**

## Project overview

UltiMedia is a LibRetro audio player and music visualizer that runs as a core plugin in frontends like RetroArch and EmuVR. It plays `.m3u` playlists and standalone `MP3`, `OGG`, `FLAC`, and `WAV` files, rendering album artwork, scrolling track info, visualizers, playback controls, shuffle state, and save-state-aware playback on a fixed **320×240, RGB565** display.

## Quick reference

| | |
|---|---|
| Language | C99 |
| Shipped artifact | `music_playlist_libretro.dll` (Windows LibRetro core) |
| Display | 320×240, RGB565 |
| Audio | 48000 Hz output, 800 samples/frame |
| Run tests | `python3 tests/run_tests.py` |
| LibRetro API | RetroArch `1.7.5` header |

## Repository layout

**Source (`src/`)** — modular C, one responsibility per file:

| File | Responsibility |
|---|---|
| `core.c` | LibRetro callbacks, playlist loading, runtime state, shuffle flow, save/load state |
| `audio.c` | Audio decoding, resampling, decoder snapshot restore |
| `audio_codecs.c` | Third-party audio decoder implementations (`dr_*`, `stb_vorbis`) |
| `video.c` | Framebuffer and display |
| `visualizer.c` | Visualizations: Bars, VU Meter, Dots, Line |
| `metadata.c` | Track metadata parsing and album-art lookup/loading |
| `image_codecs.c` | Third-party image decoder implementation (`stb_image`) |
| `config.c` | LibRetro core options (declare + read) |
| `layout.c` | Responsive UI layout computation |
| `stb_vorbis_compat.h` | Shared Vorbis declarations used by `audio.c` and `metadata.c` |
| `path_io.h` | UTF-8-aware file opening, including wide Windows paths |
| `*.h` | Module interfaces; `core_debug.h` exposes state accessors for the test harness, `core_log.h` the shared diagnostic logger |

**Tests & contributor tooling**
- `tests/run_tests.py` — generates audio/artwork/playlist fixtures, compiles the native harness, runs it
- `tests/core_harness.c` — native LibRetro harness for playback/state/navigation checks
- `tests/SMOKE_CHECKLIST.md` — manual frontend verification checklist
- `scripts/setup-windows.ps1` / `test-windows.ps1` / `build-windows.ps1` — Windows (MSYS2 UCRT64) wrappers; `windows-common.ps1` holds shared helpers

**Project metadata & docs**
- `music_playlist_libretro.info` — frontend core metadata; **keep in sync with actual capabilities** (e.g. `input_descriptors`, `savestate`, supported extensions)
- `emuvr_override_custom.cfg` — EmuVR per-folder frontend overrides
- `README.md`, `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`, `SECURITY.md`, `LICENSE` (MIT)
- `.github/workflows/` — CI (`build.yml`) plus Claude automation (`claude.yml`, `claude-code-review.yml`); `.github/ISSUE_TEMPLATE/` and `PULL_REQUEST_TEMPLATE.md`

**Third-party dependencies** — fetched into a gitignored `deps/` on demand, **not** vendored:

| Header | Source | Pin |
|---|---|---|
| `libretro.h` | libretro/RetroArch | `v1.7.5` |
| `dr_mp3.h`, `dr_wav.h`, `dr_flac.h` | mackron/dr_libs | `master` |
| `stb_image.h`, `stb_vorbis.c` | nothings/stb | `master` |

## Build, test & dependencies

Dependencies are not committed — they're fetched into `deps/` before building.

**Windows (preferred contributor path)** — checked-in PowerShell wrappers (assume MSYS2 at `C:\msys64`):

```powershell
.\scripts\setup-windows.ps1   # install UCRT64 packages + fetch deps
.\scripts\test-windows.ps1    # fetch deps (if needed) + run harness
.\scripts\build-windows.ps1   # fetch deps (if needed) + build the DLL
```

**Direct build (deps already in `deps/`)** — from an MSYS2 UCRT64 shell:

```bash
python3 tests/run_tests.py
gcc -shared -O2 -I./deps -I./src -o music_playlist_libretro.dll \
  src/core.c src/audio.c src/audio_codecs.c src/video.c src/visualizer.c \
  src/metadata.c src/image_codecs.c src/config.c src/layout.c -lm
```

**macOS / Linux** — there is no setup script here; fetch deps manually (see the `curl` block in [`CONTRIBUTING.md`](CONTRIBUTING.md)), then run `python3 tests/run_tests.py`. The harness uses `cc` by default (override with `CC=`). The distributable DLL is built on Windows; on these platforms the harness is the primary local check.

**CI (`.github/workflows/build.yml`)**
- Native tests on `windows-latest`, `ubuntu-latest`, and `macos-latest`
- Fetches and caches `deps/` automatically (cache key `deps-v2`)
- Builds the DLL on Windows (UCRT64) and uploads it as a workflow artifact
- Runs `clang --analyze` on Ubuntu, non-blocking — intentionally excludes `audio_codecs.c`/`image_codecs.c` (third-party single-header libs would flood the analyzer)
- On pushes to `main`, publishes the artifact as a GitHub release

## Code style & constraints

- **C99**, 4-space indentation
- Static globals hold state (this is a LibRetro core, not a reusable library)
- Inline comments for non-obvious logic only
- Keep functions focused and under ~50 lines where practical
- `uint16_t` for RGB565 framebuffer operations; `int16_t` for audio sample buffers
- Route diagnostics through `core_log(level, ...)` (see `core_log.h`), not raw `fprintf(stderr, ...)`, so the frontend's log level controls verbosity

```c
#define OUT_RATE 48000          // Fixed output sample rate
#define SAMPLES_PER_FRAME 800   // Audio samples per video frame
```

**Hard constraints**
- Render within **320×240, RGB565** — never assume other dimensions.
- Keep memory usage minimal (embedded/emulator context).
- New UI elements need **both** a responsive branch (positioned from `layout.*`) **and** a non-responsive fallback (positioned from the element's `media_*_y` offset). Responsive is default-On, but the Off path is wired into every drawing routine in `core.c` and `visualizer.c` — don't break it.

## LibRetro callbacks (`core.c`)

- `retro_run()` — main loop; produces audio + one video frame per call
- `retro_load_game()` — load an M3U playlist or a single audio file
- `retro_reset()` — restart the current playback session
- `retro_serialize()` / `retro_unserialize()` — save / restore playback + session state
- `retro_set_environment()` — declares core options, input descriptors, and the RGB565 pixel format
- Input is read through `retro_input_state_t`

## Configuration options (`config.c`)

All keys are prefixed `media_`, declared in `config_declare_variables()` and read in `config_update()`. Defaults in parentheses.

- **Visibility (On/Off, all On):** `media_show_art`, `media_show_txt`, `media_show_viz`, `media_show_bar`, `media_show_tim`, `media_show_ico`
- **Responsive layout:** `media_responsive` (On), `media_debug_layout` (Off), and usable-region bounds `media_ui_top` (20), `media_ui_bottom` (80), `media_ui_left` (10), `media_ui_right` (90), as percentages
- **Manual Y offsets** (used mainly when responsive is Off): `media_art_y` (40), `media_txt_y` (150), `media_viz_y` (140), `media_bar_y` (180), `media_tim_y` (190), `media_ico_y` (20)
- **Visualizer:** `media_viz_mode` (`Bars` | `VU Meter` | `Dots` | `Line`; legacy `FFT EQ` maps to `Bars`), `media_viz_bands` (40; presets 40/20), `media_viz_gradient` (On), `media_viz_peak_hold` (30)
- **Track text:** `media_use_filename` — `Show ID` (metadata) | `Show filename with extension` | `Show Filename without extension`
- **Colors:** six 0–255 channels `media_bg_r/g/b` (0/64/0) and `media_fg_r/g/b` (0/255/0), packed into `cfg.bg_rgb` / `cfg.fg_rgb` as RGB565

## Controls

| Button | Action |
|---|---|
| `B` | Pause / Play |
| `X` | Cycle visualizer (`Bars → VU Meter → Dots → Line`) |
| `L` / `R` | Previous / Next track |
| `LEFT` / `RIGHT` | Seek backward / forward ~3 seconds |
| `Y` | Toggle shuffle |

Input descriptors for these are registered via `RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS`, so labels appear in the frontend's controls menu (`input_descriptors = "true"` in the `.info`).

## Working in this repo

1. Put audio/visual logic in the matching `src/` module.
2. Validate with `python3 tests/run_tests.py` first; for changes that touch runtime UX, also walk [`tests/SMOKE_CHECKLIST.md`](tests/SMOKE_CHECKLIST.md) in a frontend.
3. Respect the 320×240 / RGB565 / minimal-memory constraints.
4. Handle both responsive and non-responsive placement for new UI elements.
5. Keep `CLAUDE.md`, `CONTRIBUTING.md`, and `music_playlist_libretro.info` consistent with what the code actually does.

**Automation boundary:** the repo has GitHub Actions that can run Claude on GitHub events (`.github/workflows/claude.yml`). For local work, do **not** auto-branch, push, open PRs, or merge unless the task explicitly asks for it.
