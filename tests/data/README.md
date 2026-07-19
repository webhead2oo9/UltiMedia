# Audio regression seeds

`restore_seed.flac` and `restore_seed.ogg` contain the same 1.25-second,
8 kHz mono non-periodic chirp. They were encoded with FFmpeg 8.1.2 and are
kept as small binary seeds because Python's standard library cannot encode
FLAC or Vorbis.

`tests/run_tests.py` copies and mutates their length metadata to exercise
save-state restoration for unknown and under-reported track lengths.

`no_xing.mp3` is a 0.287-second, 44.1 kHz stereo tone encoded with FFmpeg
8.1.2 using `-write_xing 0`. Its missing Xing/Info frame count makes
`drmp3.totalPCMFrameCount` report `UINT64_MAX`; the playlist regression test
verifies that the core obtains the real duration through the decoder API.

All PCM content is generated for this project and carries no third-party
media rights.
