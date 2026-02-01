# Music Playlist Core

A LibRetro audio player for EmuVR. Plays music with album artwork, visualizers, and scrolling track info on a virtual screen.

**Originally created by KrisRetro**

## Installation

1. Download `music_playlist_libretro.dll` from [Releases](../../releases)
2. Place it in your EmuVR `cores` folder
3. Load an M3U playlist or audio file

## Supported Formats

- **Audio:** MP3, OGG, FLAC, WAV
- **Playlists:** M3U (UTF-8 and UTF-16)

Place album art (`cover.png`, `cover.jpg`, or `folder.jpg`) in the same folder as your playlist.

## Core Options

### Display Elements
| Option | Description | Default |
|--------|-------------|---------|
| Show Art | Display album artwork | On |
| Show Scroll Text | Scrolling track/artist info | On |
| Show Visualizer | Audio visualization | On |
| Show Progress Bar | Playback progress | On |
| Show Time | Current/total time | On |
| Show Icons | Playback state icons | On |
| LCD Effect | CRT scanline overlay | On |

### Colors
Background and foreground RGB values (0-255) for UI elements.

### Visualizer
| Option | Values | Default |
|--------|--------|---------|
| Viz Mode | Bars, VU Meter, Dots, Line | Bars |
| Viz Bands | 20, 40 | 40 |
| Viz Gradient | On/Off | On |
| Peak Hold | 0-60 frames | 30 |

### Position Offsets
Adjust Y position of each element (Art, Text, Viz, Bar, Time, Icons) to customize layout.

### Other
| Option | Description | Default |
|--------|-------------|---------|
| Show Filename Only | Use filename instead of metadata | Off |

## License

MIT
