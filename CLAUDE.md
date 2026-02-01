# UltiMedia - Claude Code Guidelines

## Project Overview

UltiMedia is a LibRetro-based audio player and music visualizer that runs as a core plugin in emulators like RetroArch and EmuVR. It displays album artwork, scrolling track info, audio visualizers, and playback controls on a 320x240 pixel display.

## Architecture

**Modular C codebase** in `src/`:
- `core.c` - LibRetro callbacks and main loop
- `audio.c` - Audio decoding (MP3, FLAC, OGG, WAV)
- `video.c` - Framebuffer and display
- `visualizer.c` - Audio visualizations
- `metadata.c` - M3U parsing and track info
- `config.c` - LibRetro core options

**Header-only dependencies** (fetched at build time):
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

Builds on Windows with MSYS2/GCC. The GitHub Actions workflow handles this automatically.

Local build (MSYS2 UCRT64):
```bash
gcc -shared -O2 -I./deps -I./src -o music_playlist_libretro.dll \
  src/core.c src/audio.c src/video.c src/visualizer.c src/metadata.c src/config.c -lm
```

## Automation Mode

**IMPORTANT: This is an experimental repository with FULL AUTOMATION enabled.**

Claude MUST automatically complete the entire workflow without human intervention:

1. **Create branch** - `git checkout -b feature/description`
2. **Implement changes** - Edit files in `src/` as needed
3. **Commit and push** - `git commit` and `git push -u origin branch`
4. **Create PR** - `gh pr create` with clear description
5. **Wait for CI** - Poll `gh pr checks` until build passes
6. **Auto-merge** - `gh pr merge --squash --delete-branch`
7. **Close issue** - `gh issue close #N`

**Do NOT stop after creating a PR.** Complete the merge yourself.

For trivial one-line fixes (typos, comments), skip the PR and commit directly to main.

## When Implementing Features

1. All audio/visual code goes in the appropriate `src/` module
2. Test considerations: This is a LibRetro core - it can't be unit tested easily. Focus on code correctness.
3. Maintain the 320x240 display constraint
4. Keep memory usage minimal (embedded/emulator context)
5. New UI elements need X/Y position config variables

## LibRetro Callbacks

The core implements these LibRetro callbacks:
- `retro_run()` - Main loop (audio + video each frame)
- `retro_load_game()` - Load M3U playlist or audio file
- `retro_set_environment()` - Declare config variables
- Input via `retro_input_state_t` callbacks

## Configuration Variables

UI elements are configurable via LibRetro core options:
- Position offsets (art_y, txt_y, viz_y, bar_y, tim_y, ico_y)
- Visibility toggles (show_art, show_txt, show_viz, etc.)
- Colors (bg_rgb, fg_rgb)
- LCD scanline effect toggle
