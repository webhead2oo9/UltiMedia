$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "windows-common.ps1")

Write-Host "Bootstrapping Windows test prerequisites..."

$commands = @(
    "pacman -S --needed --noconfirm mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-python curl"
) + (Get-UltiMediaDependencyCommands) + @(
    "python3 tests/run_tests.py"
)

Invoke-UltiMediaMsys -Commands $commands

Write-Host "Windows native tests passed."
