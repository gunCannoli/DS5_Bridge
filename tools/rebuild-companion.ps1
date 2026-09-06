<#
.SYNOPSIS
  Rebuild the unpacked companion app and relaunch it in the tray.

.DESCRIPTION
  The one companion build this fork uses is the unpacked
  C:\game\DS5 Bridge App (npm run package:win:local). The packager wipes that
  directory before rebuilding, so a running instance locks DS5 Bridge.exe and
  the build fails. This script does the full cycle:

    1. Kill any running C:\game\DS5 Bridge App\DS5 Bridge.exe.
    2. npm run package:win:local  (typecheck is the caller's job; this is a
       rebuild, not a test).
    3. Relaunch "C:\game\DS5 Bridge App\DS5 Bridge.exe" --start-in-tray.

  Closing/relaunching the app without asking is covered by standing permission
  (see AGENTS.md). Run from a native PowerShell session -- package:win:local
  invokes build:firmware-tools (the flash-nuke build), which hits the same
  picotool crash as the firmware build when spawned from a Git Bash tree.

.PARAMETER NoRelaunch
  Rebuild but don't start the app afterwards.

.EXAMPLE
  .\tools\rebuild-companion.ps1
#>
param(
  [switch] $NoRelaunch
)

$ErrorActionPreference = 'Stop'

$repoRoot     = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$companionDir = Join-Path $repoRoot 'companion'
$appExe       = 'C:\game\DS5 Bridge App\DS5 Bridge.exe'

# ELECTRON_RUN_AS_NODE=1 makes every Electron binary run as plain Node -- no
# app object, no window, no tray. If it's set in this environment the packaged
# app exits immediately ("bad option: --start-in-tray" / silent exit 0). Some
# tool/CI shells export it; clear it for this script's scope so both the build
# and the relaunch behave.
if ($env:ELECTRON_RUN_AS_NODE) {
  Write-Host "note: clearing ELECTRON_RUN_AS_NODE for this run (was '$env:ELECTRON_RUN_AS_NODE')"
  Remove-Item Env:ELECTRON_RUN_AS_NODE -ErrorAction SilentlyContinue
}

function Get-CompanionProcs {
  Get-Process -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -and $_.Path -like '*DS5 Bridge App*' }
}

Write-Host "== Stopping any running companion app =="
# The running app is an Electron process tree (main + gpu/renderer/utility
# children). Killing a child just lets the main respawn it, so target the whole
# tree: taskkill /T on every DS5 Bridge.exe PID (/T takes descendants too, /F
# forces). Loop a few times in case the OS is mid-teardown.
$maxRounds = 8
for ($round = 1; $round -le $maxRounds; $round++) {
  $procs = @(Get-CompanionProcs)
  if ($procs.Count -eq 0) {
    Write-Host "  clear"
    break
  }
  Write-Host "  round ${round}: $($procs.Count) process(es) -- taskkill /T /F"
  foreach ($p in $procs) {
    & taskkill.exe /PID $p.Id /T /F 2>&1 | Out-Null
  }
  Start-Sleep -Milliseconds 700
  if ($round -eq $maxRounds -and @(Get-CompanionProcs).Count -gt 0) {
    throw ("Companion app still running after $maxRounds taskkill rounds. " +
           "Close it from the tray manually, then re-run.")
  }
}
Start-Sleep -Milliseconds 500   # final settle before the wipe

Write-Host ""
Write-Host "== Rebuilding (npm run package:win:local) =="
Push-Location $companionDir
try {
  & npm run package:win:local
  if ($LASTEXITCODE -ne 0) { throw "npm run package:win:local failed (exit $LASTEXITCODE)." }
} finally {
  Pop-Location
}

# The flash-nuke build (build:firmware-tools, run as part of package:win:local)
# regenerates companion/firmware/pico-universal-flash-nuke.uf2 with fresh
# timestamps every time, so its SHA -- and the generated
# pico-universal-flash-nuke-hash.ts that upstream tracks -- churns on every
# build. We never intend to change those; restore them so the working tree
# stays clean. (If you're deliberately updating the flash-nuke tool, git
# checkout them back and commit the new hash yourself.)
$nukeChurn = @(
  'companion/src/main/pico-universal-flash-nuke-hash.ts',
  'companion/firmware/pico-universal-flash-nuke.uf2',
  'companion/firmware/pico-universal-flash-nuke.uf2.sha256'
)
# Only touch files git actually tracks, and don't let git's exit code leak out
# of this script.
$tracked = (& git -C $repoRoot ls-files -- $nukeChurn) 2>$null
if ($tracked) {
  & git -C $repoRoot checkout -q -- $tracked 2>$null
  Write-Host "  restored non-deterministic flash-nuke outputs: $($tracked -join ', ')"
}
$global:LASTEXITCODE = 0

if (-not (Test-Path -LiteralPath $appExe)) {
  throw "Build finished but $appExe is missing."
}

if ($NoRelaunch) {
  Write-Host ""
  Write-Host "Rebuilt. Not relaunching (-NoRelaunch)."
  return
}

Write-Host ""
Write-Host "== Relaunching in tray =="
Start-Process -FilePath $appExe -ArgumentList '--start-in-tray'
Write-Host "  started: `"$appExe`" --start-in-tray"

exit 0
