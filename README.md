# UltiMedia (Music Playlist Core)

A LibRetro audio player core for EmuVR/RetroArch that plays playlists with album art, scrolling track text, and visualizers.

**Originally created by KrisRetro**

## Quick Start

1. Download `music_playlist_libretro.dll` from [Releases](../../releases)
2. Place it in your `cores` folder
3. Load an `.m3u` playlist (or a single audio file)

## What It Can Do

- Play `MP3`, `OGG`, `FLAC`, and `WAV`
- Read `M3U` playlists (UTF-8 and UTF-16)
- Parse metadata from MP3, OGG, and FLAC tags
- Show album art from nearby image files or embedded artwork
- Display 5 visualizer modes: `Bars`, `FFT EQ`, `VU Meter`, `Dots`, `Line`
- Auto-arrange UI with responsive layout bounds

## Controls

- `B`: Pause/Play
- `X`: Cycle visualizer mode (`Bars -> FFT EQ -> VU Meter -> Dots -> Line`)
- `L` / `R`: Previous / Next track
- `LEFT` / `RIGHT`: Seek backward / forward
- `Y`: Toggle shuffle

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
- Show Icons

### Visualizer

- Viz Mode: `Bars`, `FFT EQ`, `VU Meter`, `Dots`, `Line`
- Viz Bands: `20` or `40`
- Viz Gradient: `On/Off`
- Peak Hold: `0` to `60` (default `30`)

### Track Text

- Track Text Mode:
  - `Show ID` (metadata when available)
  - `Show filename with extension`
  - `Show Filename without extension`

### Responsive Layout

- Responsive Layout: `On/Off` (default `On`)
- UI Top / Bottom / Left / Right (%): defines the usable screen region
- Debug Layout Bounds: `Off/On`
  - Draws colored layout boxes to help tune responsive positioning

### Manual Y Offsets (mainly for non-responsive mode)

- Art Y
- Text Y
- Viz Y
- Bar Y
- Time Y
- Icon Y

### Colors

- BG Red / Green / Blue
- FG Red / Green / Blue

All color channels are `0-255`.

## Notes for Playlists

- Relative paths are recommended for portability
- Absolute paths also work if valid on the current machine
- `file://` playlist entries are supported

## Compatibility

- Designed for LibRetro frontends (RetroArch/EmuVR)
- Intended to work on RetroArch `1.7.5` and newer

## License

MIT
