# UltiMedia (Music Playlist Core)

![UltiMedia logo](branding/logo.png)

[![Build & Release](https://github.com/webhead2oo9/UltiMedia/actions/workflows/build.yml/badge.svg)](https://github.com/webhead2oo9/UltiMedia/actions/workflows/build.yml)
[![Latest release](https://img.shields.io/github/v/release/webhead2oo9/UltiMedia?display_name=tag&label=release)](https://github.com/webhead2oo9/UltiMedia/releases/latest)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
![Platforms](https://img.shields.io/badge/platforms-Windows%20%C2%B7%20Linux%20%C2%B7%20macOS-lightgrey)

A LibRetro audio player core for RetroArch and EmuVR that plays `.m3u` playlists and standalone audio files with album art, scrolling track text, responsive layout, shuffle, and save-state-aware playback.

## Quick Start

1. Download `music_playlist_libretro.dll` from [Releases](../../releases)
2. Place `music_playlist_libretro.dll` in your RetroArch `cores` folder
3. For EmuVR content folders, also place `emuvr_override_custom.cfg` beside the `.m3u` playlist and music files
4. Load an `.m3u` playlist, or load a single supported audio file directly

## EmuVR Override File

`emuvr_override_custom.cfg` is meant for EmuVR content folders. Put it beside the playlist/music you want this core to handle.

It applies these RetroArch frontend settings:

```ini
video_shader = "shaders\\shaders_glsl\\stock.glslp"
builtin_imageviewer_enable = "false"
builtin_mediaplayer_enable = "false"
```

This keeps EmuVR/RetroArch from handing images or media to the built-in viewers instead of this core, while also applying the stock shader.

## What It Can Do

- Play `MP3`, `OGG`, `FLAC`, and `WAV`
- Read `M3U` playlists (UTF-8 and UTF-16)
- Parse metadata from MP3, OGG, and FLAC tags
- Show album art from nearby image files or embedded artwork
- Display 7 visualizer modes: `Bars`, `VU Meter`, `Dots`, `Line`, `Scope`, `Mirror`, `Horizon`
- Auto-arrange UI with responsive layout bounds
- Preserve playback, pause, and shuffle state through LibRetro save states

## Controls

- `B`: Pause/Play
- `X`: Cycle visualizer mode (`Bars -> VU Meter -> Dots -> Line -> Scope -> Mirror -> Horizon`)
- `L` / `R`: Previous / Next track
- `LEFT` / `RIGHT`: Seek backward / forward by about 3 seconds
- `Y`: Toggle shuffle

RetroArch input descriptors are registered for these actions, so they should be labeled cleanly in the controls menu.

## Album Art Search Order

When a track loads, art is searched in this order:

1. Same filename as the track (different image extension)
2. Same name as the parent folder
3. Same name as album metadata tag
4. Same filename as the loaded `.m3u`
5. Embedded image scan in the audio file

## Core Options (Easy Version)

### Display Toggles

- Show Art
- Show Scroll Text
- Show Visualizer
- Show Progress Bar
- Show Time
- Show Transport Icons (also auto-hides on its own when the layout gets too short)

### Visualizer

- Viz Mode: `Bars`, `VU Meter`, `Dots`, `Line`, `Scope`, `Mirror`, `Horizon`
- Viz Bands: `20` or `40`
- Viz Gradient: `On/Off`
- Peak Hold presets: `0`, `15`, `30`, `45`, `60` (default `30`)

### Track Text

- Track Text Mode:
  - `Show ID` (metadata when available)
  - `Show filename with extension`
  - `Show Filename without extension`

### Layout

- UI Top / Bottom / Left / Right (%): defines the usable screen region
- Debug Layout Bounds: `Off/On`
  - Draws colored layout boxes to help tune positioning

### Colors

- BG Red / Green / Blue
- FG Red / Green / Blue

All color channels are `0-255`.

## Notes for Playlists

- Relative paths are recommended for portability
- Absolute paths also work if valid on the current machine
- `file://` playlist entries are supported
- For EmuVR, keep `emuvr_override_custom.cfg` in the same folder as the playlist/music content

## Compatibility

- Designed for LibRetro frontends (RetroArch/EmuVR)
- Intended to work on RetroArch `1.7.5` and newer
- The repo pins `libretro.h` fetches to RetroArch `1.7.5` in CI and local setup instructions

## Testing

Run the native test harness locally with:

```bash
python3 tests/run_tests.py
```

This command generates temporary audio, artwork, and playlist fixtures at runtime (using the small FLAC/Vorbis seeds in `tests/data/` for decoder-length edge cases), compiles a native harness, and exercises playback, UTF-8/UTF-16 playlist navigation, seeking, bad-track handling, artwork scaling, shuffle, reset, and save-state behavior.

Set `CFLAGS` to add compiler instrumentation or diagnostics to the harness build, for example `CFLAGS="-fsanitize=address,undefined"` on a supported compiler.

Windows local testing is supported through `MSYS2 UCRT64`. On macOS and Linux, the runner uses the system `cc` compiler by default.

### Windows Contributor Setup

The repo ships PowerShell wrappers for the Windows contributor path. They assume `MSYS2` is installed at `C:\msys64`.

Run these from PowerShell at the repo root:

```powershell
.\scripts\setup-windows.ps1
.\scripts\test-windows.ps1
.\scripts\build-windows.ps1
```

What they do:

- install the `MSYS2 UCRT64` packages used by CI
- fetch the pinned third-party dependencies used by the repo
- run the native test harness or build the core with the same toolchain family as CI

### Local Build

If you prefer working inside an MSYS2 shell directly, open the `UCRT64` shell. Make sure the pinned dependencies are already present in `deps/` first, or run `.\scripts\setup-windows.ps1` once from PowerShell, then run:

```bash
python3 tests/run_tests.py
gcc -shared -O2 -I./deps -I./src -o music_playlist_libretro.dll \
  src/core.c src/audio.c src/audio_codecs.c src/video.c src/visualizer.c \
  src/metadata.c src/image_codecs.c src/config.c src/layout.c src/ui.c -lm
```

### CI Coverage

The main GitHub Actions workflow currently validates:

- native tests on `windows-latest`, `ubuntu-latest`, and `macos-latest`
- Windows UCRT64 DLL builds
- optional `clang --analyze` static analysis on Ubuntu

Pushes to `main` also publish the built DLL as a GitHub release artifact.

For manual frontend verification, use [tests/SMOKE_CHECKLIST.md](tests/SMOKE_CHECKLIST.md).

## Contributing

Contributions are welcome! See [CONTRIBUTING.md](CONTRIBUTING.md) for dev setup, the build and test workflow, and code style. Please also review the [Code of Conduct](CODE_OF_CONDUCT.md). To report a security issue, see [SECURITY.md](SECURITY.md).

## License

Released under the [MIT License](LICENSE).

## Acknowledgements

- Originally created by **KrisRetro**
- [LibRetro](https://www.libretro.com/) — core API (`libretro.h`)
- [dr_libs](https://github.com/mackron/dr_libs) by David Reid — `dr_mp3`, `dr_wav`, and `dr_flac` audio decoders
- [stb](https://github.com/nothings/stb) by Sean Barrett — `stb_image` and `stb_vorbis`
