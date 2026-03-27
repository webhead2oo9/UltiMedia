$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "windows-common.ps1")

Write-Host "Preparing MSYS2 UCRT64 toolchain and repo dependencies..."

$commands = @(
    "pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-python curl"
) + (Get-UltiMediaDependencyCommands)

Invoke-UltiMediaMsys -Commands $commands

Write-Host "Windows contributor environment is ready."
