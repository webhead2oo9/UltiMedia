$ErrorActionPreference = "Stop"

function Get-UltiMediaRepoRoot {
    return Split-Path -Parent $PSScriptRoot
}

function Convert-ToMsysPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$WindowsPath
    )

    $resolved = (Resolve-Path $WindowsPath).Path
    $drive = $resolved.Substring(0, 1).ToLowerInvariant()
    $suffix = $resolved.Substring(2).Replace("\", "/")
    return "/$drive$suffix"
}

function Get-UltiMediaDependencyCommands {
    return @(
        "mkdir -p deps",
        "[ -f deps/libretro.h ] || curl -sL -o deps/libretro.h https://raw.githubusercontent.com/libretro/RetroArch/v1.7.5/libretro-common/include/libretro.h",
        "[ -f deps/dr_mp3.h ] || curl -sL -o deps/dr_mp3.h https://raw.githubusercontent.com/mackron/dr_libs/master/dr_mp3.h",
        "[ -f deps/dr_wav.h ] || curl -sL -o deps/dr_wav.h https://raw.githubusercontent.com/mackron/dr_libs/master/dr_wav.h",
        "[ -f deps/dr_flac.h ] || curl -sL -o deps/dr_flac.h https://raw.githubusercontent.com/mackron/dr_libs/master/dr_flac.h",
        "[ -f deps/stb_image.h ] || curl -sL -o deps/stb_image.h https://raw.githubusercontent.com/nothings/stb/master/stb_image.h",
        "[ -f deps/stb_vorbis.c ] || curl -sL -o deps/stb_vorbis.c https://raw.githubusercontent.com/nothings/stb/master/stb_vorbis.c"
    )
}

function Invoke-UltiMediaMsys {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Commands
    )

    $bashExe = "C:\msys64\usr\bin\bash.exe"
    if (-not (Test-Path $bashExe)) {
        throw "MSYS2 bash was not found at $bashExe. Install MSYS2 to C:\msys64 first."
    }

    $repoRoot = Get-UltiMediaRepoRoot
    $repoPosix = Convert-ToMsysPath $repoRoot
    $allCommands = @(
        "set -euo pipefail",
        "export MSYSTEM=UCRT64",
        "export PATH=/ucrt64/bin:/usr/bin:`$PATH",
        "cd '$repoPosix'"
    ) + $Commands

    & $bashExe -lc ($allCommands -join "; ")
    if ($LASTEXITCODE -ne 0) {
        throw "MSYS2 command failed with exit code $LASTEXITCODE."
    }
}
