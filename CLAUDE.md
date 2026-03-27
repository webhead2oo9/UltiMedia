# UltiMedia - Claude Code Guidelines

## Project Overview

UltiMedia is a LibRetro-based audio player and music visualizer that runs as a core plugin in frontends like RetroArch and EmuVR. It plays `.m3u` playlists and standalone `MP3`, `OGG`, `FLAC`, and `WAV` files, and renders album artwork, scrolling track info, visualizers, playback controls, shuffle state, and save-state-aware playback on a 320x240 display.

## Architecture

**Modular C codebase** in `src/`:
- `core.c` - LibRetro callbacks, playlist loading, runtime state, shuffle flow, save/load state
- `audio.c` - Audio decoding, resampling, and decoder snapshot restore
- `audio_codecs.c` - Third-party audio decoder implementations (`dr_*` and `stb_vorbis`)
- `video.c` - Framebuffer and display
- `visualizer.c` - Audio visualizations (`Bars`, `VU Meter`, `Dots`, `Line`)
- `metadata.c` - Track metadata parsing and album art lookup/loading
- `image_codecs.c` - Third-party image decoder implementation (`stb_image`)
- `stb_vorbis_compat.h` - Shared Vorbis declarations used by `audio.c` and `metadata.c`
- `config.c` - LibRetro core options
- `layout.c` - Responsive UI layout computation

**Test and contributor support**:
- `tests/core_harness.c` - Native LibRetro harness for playback/state/navigation checks
- `tests/run_tests.py` - Fixture generation + harness compile/run entry point
- `tests/SMOKE_CHECKLIST.md` - Manual frontend verification checklist
- `scripts/setup-windows.ps1` / `test-windows.ps1` / `build-windows.ps1` - Windows contributor wrappers around the MSYS2 UCRT64 path

**Fetched third-party dependencies**:
- `libretro.h` - LibRetro core API
- `dr_mp3.h` / `dr_wav.h` / `dr_flac.h` - Audio decoders (Dr. Libs)
- `stb_image.h` - Image loading
- `stb_vorbis.c` - OGG Vorbis decoder

**Output:** `music_playlist_libretro.dll` (Windows DLL for LibRetro)

## Code Style

- C99 standard
- 4-space indentation
- Static globals for state (this is a LibRetro core, not a library)
- Inline comments for non-obvious logic
- Keep functions focused and under 50 lines when practical
- Use `uint16_t` for RGB565 framebuffer operations
- Use `int16_t` for audio sample buffers

## Key Constants

```c
#define OUT_RATE 48000          // Fixed output sample rate
#define SAMPLES_PER_FRAME 800   // Audio samples per video frame
```

Display: 320x240 pixels, RGB565 format

## Build

Builds on Windows with MSYS2/GCC. The repo targets the RetroArch `1.7.5` LibRetro API header in both CI and local dependency bootstrap.

Local build/test on Windows:
- Preferred contributor path is the checked-in PowerShell wrappers:
  - `.\scripts\setup-windows.ps1`
  - `.\scripts\test-windows.ps1`
  - `.\scripts\build-windows.ps1`
- These use `MSYS2 UCRT64`, install the CI-equivalent packages, fetch the pinned third-party dependencies into `deps/`, run the native test harness, and build the DLL.

Direct MSYS2 UCRT64 build:
- Assumes the pinned dependencies have already been fetched into `deps/`
```bash
python3 tests/run_tests.py
gcc -shared -O2 -I./deps -I./src -o music_playlist_libretro.dll \
  src/core.c src/audio.c src/audio_codecs.c src/video.c src/visualizer.c \
  src/metadata.c src/image_codecs.c src/config.c src/layout.c -lm
```

GitHub Actions build (`.github/workflows/build.yml`):
- Runs native tests on `windows-latest`, `ubuntu-latest`, and `macos-latest`
- Uses `msys2/setup-msys2@v2` with `msystem: UCRT64` for Windows jobs
- Installs `mingw-w64-ucrt-x86_64-gcc`, `python`, and `curl` as needed
- Caches and fetches the `deps/` headers automatically
- Builds the same `music_playlist_libretro.dll` target with the same source list
- Uploads the DLL as a workflow artifact
- Runs `clang --analyze` on Ubuntu as non-blocking static analysis
- On pushes to `main`, the release job downloads that artifact and publishes a GitHub release

## Repo Automation

The repo includes GitHub Actions workflows that can run Claude in an automated mode on GitHub events. That workflow behavior is defined in `.github/workflows/claude.yml`.

Do not assume local work should always auto-branch, push, create PRs, or merge unless the current task explicitly calls for that workflow.

## When Implementing Features

1. All audio/visual code goes in the appropriate `src/` module
2. Test considerations: use `tests/run_tests.py` first for code-level validation, then use `tests/SMOKE_CHECKLIST.md` for frontend smoke testing when behavior changes touch runtime UX
3. Maintain the 320x240 display constraint
4. Keep memory usage minimal (embedded/emulator context)
5. New UI elements usually need both responsive layout handling and non-responsive fallback placement

## LibRetro Callbacks

The core implements these LibRetro callbacks:
- `retro_run()` - Main loop (audio + video each frame)
- `retro_load_game()` - Load M3U playlist or audio file
- `retro_reset()` - Restart the current playback session
- `retro_serialize()` / `retro_unserialize()` - Save and restore playback/session state
- `retro_set_environment()` - Declare config variables, input descriptors, and pixel format
- Input via `retro_input_state_t` callbacks

## Configuration Variables

UI elements are configurable via LibRetro core options:
- Position offsets (`art_y`, `txt_y`, `viz_y`, `bar_y`, `tim_y`, `ico_y`)
- Visibility toggles (`show_art`, `show_txt`, `show_viz`, `show_bar`, `show_tim`, `show_ico`)
- Responsive layout controls (`responsive`, `debug_layout`, `ui_top`, `ui_bottom`, `ui_left`, `ui_right`)
- Visualizer options (`viz_mode`, `viz_bands`, `viz_gradient`, `viz_peak_hold`)
- Track text mode (`media_use_filename`)
- Colors (`bg_rgb`, `fg_rgb`)

## Controls

Current user-facing controls:
- `B` - Pause/Play
- `X` - Cycle visualizer mode (`Bars -> VU Meter -> Dots -> Line`)
- `L` / `R` - Previous / Next track
- `LEFT` / `RIGHT` - Seek backward / forward by about 3 seconds
- `Y` - Toggle shuffle

RetroArch input descriptors are registered for these actions, so control labels should appear in the frontend controls menu.
