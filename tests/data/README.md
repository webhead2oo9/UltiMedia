# Audio restore seeds

`restore_seed.flac` and `restore_seed.ogg` contain the same 1.25-second,
8 kHz mono non-periodic chirp. They were encoded with FFmpeg 8.1.2 and are
kept as small binary seeds because Python's standard library cannot encode
FLAC or Vorbis.

`tests/run_tests.py` copies and mutates their length metadata to exercise
save-state restoration for unknown and under-reported track lengths. The
PCM content itself is generated for this project and carries no third-party
media rights.
