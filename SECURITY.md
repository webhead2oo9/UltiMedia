# Security Policy

## Supported versions

UltiMedia is distributed as rolling builds, with the most recent core published on the [Releases](https://github.com/webhead2oo9/UltiMedia/releases) page. Only the latest release is supported; please reproduce any issue against the latest build before reporting.

| Version | Supported |
| ------- | --------- |
| Latest release | :white_check_mark: |
| Older builds | :x: |

## Reporting a vulnerability

This core parses untrusted input — playlists (`.m3u`), audio files (`MP3`, `OGG`, `FLAC`, `WAV`), and image files for album art — using third-party decoders. Malformed media is the most likely source of memory-safety issues, so reports in that area are especially welcome.

Please **do not** open a public issue for security problems. Instead, report privately through GitHub:

1. Go to the [Security advisories](https://github.com/webhead2oo9/UltiMedia/security/advisories/new) page.
2. Click **Report a vulnerability** and describe the issue, including a sample file or playlist that reproduces it when possible.

You can expect an initial response within a reasonable time frame. Once a fix is available, it will ship in a new release.
