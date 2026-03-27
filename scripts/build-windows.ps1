$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "windows-common.ps1")

Write-Host "Bootstrapping Windows build prerequisites..."

$commands = @(
    "pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-python curl"
) + (Get-UltiMediaDependencyCommands) + @(
    "gcc -shared -O2 -I./deps -I./src -o music_playlist_libretro.dll src/core.c src/audio.c src/audio_codecs.c src/video.c src/visualizer.c src/metadata.c src/image_codecs.c src/config.c src/layout.c -lm"
)

Invoke-UltiMediaMsys -Commands $commands

$output = Join-Path (Get-UltiMediaRepoRoot) "music_playlist_libretro.dll"
Write-Host "Built $output"
