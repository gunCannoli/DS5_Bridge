<#
.SYNOPSIS
  The single firmware build path for this fork (Waveshare RP2350B-Plus-W only).

.DESCRIPTION
  This fork ships only the Waveshare RP2350B-Plus-W target -- that's the board
  the Wake-on-LAN feature needs (CYW43 Wi-Fi + lwIP). There is no stock-board
  build here; if a shared-code change needs a non-Waveshare sanity compile
  before going upstream, do it in a throwaway dir and delete it.

  Everything builds in ONE place: build\waveshare\ (CMake work dir, always
  reconfigured in place). The finished UF2 is then staged into firmware\ at the
  repo root as:

      ds5-bridge-<version>-wol-<variant>.uf2

  e.g. ds5-bridge-1.71-wol-final.uf2 -- so every UF2 ever built is in one
  folder, self-identifying by version and variant. Both build\ and firmware\
  are gitignored.

  Launch this from a native PowerShell session (the project's primary shell).
  picotool's post-link UF2 conversion segfaults whenever it runs inside a Git
  Bash process tree on this machine -- including powershell.exe spawned from Git
  Bash, since the broken environment is inherited (see AGENTS.md "Local build
  environment notes"). From a natively-started PowerShell session it's reliable.
  This script still keeps a belt-and-suspenders recovery step that re-runs
  picotool if CMake's post-link chain didn't produce a fresh UF2, for the case
  where some other post-link tool (objcopy) trips instead.

.PARAMETER Variant
  final  (default) Release, no diagnostics, host-alive gate active. Shippable.
  debug            Release + DS5_DIAGNOSTICS_PRESET=all: 921600-baud UART log of
                   every WOL/BT state transition. Host-alive gate still active.
                   Use after a failed smoke test when you need the trace.
  smoke            final + -DWOL_ALWAYS=ON: skips the host-alive gate so WOL
                   fires on every controller connect regardless of PC power
                   state. Bring-up testing only.

.PARAMETER PicoSdkPath
  Overrides $env:PICO_SDK_PATH. Must be pico-sdk 2.3.0 with TinyUSB at
  2d56dc533e45e4e91b15e93fdab5e22e964f328d.

.EXAMPLE
  .\tools\build-firmware.ps1
  .\tools\build-firmware.ps1 debug
#>
param(
  [ValidateSet('final', 'debug', 'smoke')]
  [string] $Variant = 'final',
  [string] $PicoSdkPath
)

$ErrorActionPreference = 'Stop'

$repoRoot  = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$buildDir  = Join-Path $repoRoot 'build\waveshare'
$stageDir  = Join-Path $repoRoot 'firmware'
$rawElf    = Join-Path $buildDir 'ds5-bridge.elf'
$rawUf2    = Join-Path $buildDir 'ds5-bridge.uf2'

if (-not $PicoSdkPath) { $PicoSdkPath = $env:PICO_SDK_PATH }
if (-not $PicoSdkPath) {
  throw 'PICO_SDK_PATH is not set. Use pico-sdk 2.3.0 with TinyUSB 2d56dc533e45e4e91b15e93fdab5e22e964f328d.'
}

switch ($Variant) {
  'final' { $diagPreset = 'off'; $wolAlways = 'OFF' }
  'debug' { $diagPreset = 'all'; $wolAlways = 'OFF' }
  'smoke' { $diagPreset = 'off'; $wolAlways = 'ON'  }
}

# Firmware version, e.g. "1.7.1" -> "1.71" for the filename.
$rawVersion = (Get-Content (Join-Path $repoRoot 'firmware-version.txt') -Raw).Trim()
$vParts     = $rawVersion.Split('.')
$fileVersion = "$($vParts[0]).$($vParts[1])$($vParts[2])"

Write-Host "Building Waveshare RP2350B-Plus-W  |  version=$rawVersion  variant=$Variant"
Write-Host "  diagnostics preset : $diagPreset"
Write-Host "  WOL_ALWAYS         : $wolAlways"
Write-Host ""

$prevUf2Mtime = [datetime]::MinValue
if (Test-Path -LiteralPath $rawUf2) {
  $prevUf2Mtime = (Get-Item -LiteralPath $rawUf2).LastWriteTimeUtc
}

# -DENABLE_COMPANION=ON is required for a real build -- it compiles
# companion.cpp, including every SET_WOL_* command handler. CMake default is
# OFF. -DPICO_NO_COPRO_DIS=1 matches the manual invocation used for the v1.7.0
# merge builds.
& cmake -S $repoRoot -B $buildDir -G Ninja `
  -DCMAKE_BUILD_TYPE=Release `
  -DWAVESHARE_RP2350B_PLUS_W_BUILD=ON `
  -DENABLE_COMPANION=ON `
  -DPICO_NO_COPRO_DIS=1 `
  "-DDS5_DIAGNOSTICS_PRESET=$diagPreset" `
  "-DWOL_ALWAYS=$wolAlways" `
  "-DPICO_SDK_PATH=$PicoSdkPath"
if ($LASTEXITCODE -ne 0) { throw "CMake configure failed (exit $LASTEXITCODE)." }

# The compile+link step is reliable. From a native PowerShell session the
# picotool/objcopy post-link chain is fine too, but keep a guard: if it didn't
# produce a fresh UF2 (objcopy tripped, or this got launched from a bad process
# tree after all), check the ELF and recover the UF2 rather than aborting.
& cmake --build $buildDir --target ds5-bridge
$buildRc = $LASTEXITCODE

if (-not (Test-Path -LiteralPath $rawElf)) {
  throw "Build failed before producing ds5-bridge.elf (exit $buildRc) -- real compile/link error."
}

$newUf2Mtime = [datetime]::MinValue
if (Test-Path -LiteralPath $rawUf2) {
  $newUf2Mtime = (Get-Item -LiteralPath $rawUf2).LastWriteTimeUtc
}

if ($buildRc -ne 0 -or $newUf2Mtime -le $prevUf2Mtime) {
  Write-Host ""
  Write-Host "Post-link step did not produce a fresh UF2 (known picotool crash) -- recovering it."
  $picotool = Join-Path $buildDir '_deps\picotool\picotool.exe'
  if (-not (Test-Path -LiteralPath $picotool)) {
    throw "Cannot find $picotool -- run the build once so CMake fetches picotool, then re-run."
  }
  Push-Location $buildDir
  try {
    & $picotool uf2 convert --quiet 'ds5-bridge.elf' 'ds5-bridge.uf2' --family rp2350-arm-s --abs-block
    if ($LASTEXITCODE -ne 0) { throw "picotool uf2 convert failed (exit $LASTEXITCODE)." }
  } finally {
    Pop-Location
  }
  $newUf2Mtime = (Get-Item -LiteralPath $rawUf2).LastWriteTimeUtc
  if ($newUf2Mtime -le $prevUf2Mtime) {
    throw "UF2 recovery failed -- ds5-bridge.uf2 is still stale."
  }
  Write-Host "UF2 recovered."
}

New-Item -ItemType Directory -Path $stageDir -Force | Out-Null
$stagedUf2 = Join-Path $stageDir "ds5-bridge-$fileVersion-wol-$Variant.uf2"
Copy-Item -LiteralPath $rawUf2 -Destination $stagedUf2 -Force

Write-Host ""
Write-Host "Build complete."
Write-Host "  raw output : $rawUf2"
Write-Host "  staged     : $stagedUf2"
