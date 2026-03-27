#!/usr/bin/env python3

import math
import os
import platform
import shutil
import shlex
import struct
import subprocess
import sys
import tempfile
import wave
from pathlib import Path


def fail(message: str) -> int:
    print(message, file=sys.stderr)
    return 1


def in_msys() -> bool:
    msystem = os.environ.get("MSYSTEM", "")
    system = platform.system().lower()
    return bool(msystem) or system.startswith("msys") or system.startswith("mingw")


def default_compiler() -> str:
    if in_msys() or os.name == "nt":
        return "gcc"
    return "cc"


def ensure_supported_windows_path() -> None:
    if os.name == "nt" and not in_msys():
        raise RuntimeError(
            "Windows local runs are supported through MSYS2 UCRT64. "
            "Run `.\\scripts\\test-windows.ps1` from PowerShell, or open an MSYS2 UCRT64 shell "
            "and run `python3 tests/run_tests.py` there."
        )


def write_wave(path: Path, frequency: float, duration_seconds: float = 2.0, sample_rate: int = 48_000) -> None:
    frame_count = int(sample_rate * duration_seconds)
    amplitude = 12_000
    with wave.open(str(path), "wb") as wav_file:
        wav_file.setnchannels(2)
        wav_file.setsampwidth(2)
        wav_file.setframerate(sample_rate)
        frames = bytearray()
        for index in range(frame_count):
            sample = int(amplitude * math.sin(2.0 * math.pi * frequency * index / sample_rate))
            frames.extend(struct.pack("<hh", sample, sample))
        wav_file.writeframes(bytes(frames))


def write_playlist(path: Path, entries: list[str]) -> None:
    path.write_text("\n".join(entries) + "\n", encoding="utf-8")


def generate_fixtures(fixtures_dir: Path) -> None:
    fixtures_dir.mkdir(parents=True, exist_ok=True)

    tracks = {
        "track_a.wav": 440.0,
        "track_b.wav": 554.37,
        "track_c.wav": 659.25,
        "track_d.wav": 880.0,
        "alt_1.wav": 329.63,
        "alt_2.wav": 493.88,
    }
    for name, frequency in tracks.items():
        write_wave(fixtures_dir / name, frequency)

    write_playlist(
        fixtures_dir / "playlist_main.m3u",
        ["track_a.wav", "track_b.wav", "track_c.wav", "track_d.wav"],
    )
    write_playlist(
        fixtures_dir / "playlist_alt.m3u",
        ["alt_1.wav", "alt_2.wav"],
    )


def build_harness(repo_root: Path, temp_dir: Path, compiler: str) -> Path:
    exe_name = "core_harness.exe" if in_msys() or os.name == "nt" else "core_harness"
    exe_path = temp_dir / exe_name
    sources = [
        "tests/core_harness.c",
        "src/core.c",
        "src/audio.c",
        "src/audio_codecs.c",
        "src/video.c",
        "src/visualizer.c",
        "src/metadata.c",
        "src/image_codecs.c",
        "src/config.c",
        "src/layout.c",
    ]
    command = [
        compiler,
        "-std=c99",
        "-O2",
        "-I./deps",
        "-I./src",
        "-o",
        str(exe_path),
        *sources,
        "-lm",
    ]
    print("Compiling harness:")
    print(shlex.join(command))
    subprocess.run(command, cwd=repo_root, check=True)
    return exe_path


def run_harness(exe_path: Path, fixtures_dir: Path, repo_root: Path) -> None:
    command = [str(exe_path), str(fixtures_dir)]
    print("Running harness:")
    print(shlex.join(command))
    subprocess.run(command, cwd=repo_root, check=True)


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent

    try:
        ensure_supported_windows_path()
    except RuntimeError as exc:
        return fail(str(exc))

    compiler = os.environ.get("CC", default_compiler())
    if shutil.which(compiler) is None:
        return fail(
            f"Compiler `{compiler}` was not found. "
            "Set CC=/path/to/compiler or install the supported toolchain."
        )

    try:
        with tempfile.TemporaryDirectory(prefix="ultimedia-tests-") as temp_name:
            temp_dir = Path(temp_name)
            fixtures_dir = temp_dir / "fixtures"
            generate_fixtures(fixtures_dir)
            exe_path = build_harness(repo_root, temp_dir, compiler)
            run_harness(exe_path, fixtures_dir, repo_root)
    except subprocess.CalledProcessError as exc:
        return fail(f"Command failed with exit code {exc.returncode}.")

    print("All native tests passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
