# Manual Smoke Test Checklist

Use this checklist before a release or after changes to playback, state handling, input, layout, or visualizer behavior.

## Environment

- Launch the core in RetroArch or EmuVR with a known-good music file and playlist.
- If testing EmuVR content, confirm `emuvr_override_custom.cfg` is present beside the playlist and audio files.

## Playback

1. Load a standalone audio file.
2. Confirm audio begins immediately and continues without stutter.
3. Confirm the progress bar and time display advance while playback is running.
4. Pause and resume playback once to confirm input responsiveness and state consistency.

## Playlist Navigation

1. Load an `.m3u` playlist with multiple tracks.
2. Advance to the next track and confirm the displayed track information updates correctly.
3. Return to the previous track and confirm navigation is accurate.
4. Verify previous from the first track wraps to the last track.
5. Verify next from the last track wraps to the first track when shuffle is disabled.

## Shuffle and Session State

1. Enable shuffle and advance through several tracks.
2. Confirm the same track does not repeat immediately and shuffle remains enabled across track changes.
3. Press previous after at least two shuffled advances and confirm it returns to the actual previously visited shuffled track.
4. Let a shuffled track finish naturally, then press previous and confirm it returns to the track that just finished.
5. Trigger frontend reset and confirm the current track restarts from the beginning.
6. Confirm shuffle remains enabled after reset when it was enabled beforehand.

## Save State

1. Start playback and wait until the track position has clearly advanced.
2. Create a save state.
3. Continue playback or change tracks so the runtime state is visibly different.
4. Load the save state.
5. Confirm the active track, playback position, pause state, and shuffle state are restored correctly.
6. Repeat once while paused to confirm paused-state restore behaves correctly.

## Presentation

1. Confirm album art loads when expected.
2. Confirm scrolling text updates with the active track and remains readable.
3. Cycle visualizer modes and confirm each mode renders without corruption or layout issues.
4. Confirm the selected layout still appears correct in the intended frontend and aspect ratio.

## Expected Result

- Playback, navigation, reset, and save-state flows complete without crashes, audio dropouts, or incorrect state restoration.
- Track metadata, artwork, and visualizer output remain synchronized with the active track.
