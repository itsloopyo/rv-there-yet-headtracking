#requires -Version 5.1
<#
Reversibly disables RV There Yet's startup movies and splash for faster
iteration during head-tracking debugging.

Renames everything with a .htdebug-disabled suffix so we can restore
verbatim. UE5's MoviePlayer treats missing startup movies as a no-op
(skips straight to the loaded level), and a missing Splash.bmp falls
back to a blank window.

The startup-movie filenames are game-specific, so the target list is built
by enumerating whatever lives under <GameRoot>\Movies plus the splash bitmap
rather than hardcoding names.

Usage:
  pixi run powershell -ExecutionPolicy Bypass -File scripts/fast-debug-load.ps1 disable
  pixi run powershell -ExecutionPolicy Bypass -File scripts/fast-debug-load.ps1 enable
  pixi run powershell -ExecutionPolicy Bypass -File scripts/fast-debug-load.ps1 status
#>

param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('disable', 'enable', 'status')]
    [string]$Action,

    [string]$GameRoot = 'C:\XboxGames\RV There Yet\Content\Ride\Content'
)

$ErrorActionPreference = 'Stop'
$Suffix = '.htdebug-disabled'

if (-not (Test-Path $GameRoot)) {
    throw "Game content root not found: $GameRoot"
}

# Collect canonical (enabled-name) targets: every file under Movies\ plus the
# splash bitmap. A file already disabled contributes its stripped canonical
# name so enable/status still see it.
$targets = @()
$moviesDir = Join-Path $GameRoot 'Movies'
if (Test-Path $moviesDir) {
    foreach ($f in Get-ChildItem -LiteralPath $moviesDir -File) {
        $canonical = if ($f.Name.EndsWith($Suffix)) {
            $f.FullName.Substring(0, $f.FullName.Length - $Suffix.Length)
        } else {
            $f.FullName
        }
        if ($targets -notcontains $canonical) { $targets += $canonical }
    }
}
$splash = Join-Path $GameRoot 'Splash\Splash.bmp'
if ((Test-Path $splash) -or (Test-Path "$splash$Suffix")) {
    if ($targets -notcontains $splash) { $targets += $splash }
}

switch ($Action) {
    'disable' {
        foreach ($t in $targets) {
            $disabled = "$t$Suffix"
            if (Test-Path $t) {
                Move-Item -LiteralPath $t -Destination $disabled -Force
                Write-Host "DISABLED: $t"
            }
            elseif (Test-Path $disabled) {
                Write-Host "already disabled: $t"
            }
            else {
                Write-Host "missing (skipped): $t"
            }
        }
    }
    'enable' {
        foreach ($t in $targets) {
            $disabled = "$t$Suffix"
            if (Test-Path $disabled) {
                Move-Item -LiteralPath $disabled -Destination $t -Force
                Write-Host "RESTORED: $t"
            }
            elseif (Test-Path $t) {
                Write-Host "already enabled: $t"
            }
            else {
                Write-Host "missing (skipped): $t"
            }
        }
    }
    'status' {
        foreach ($t in $targets) {
            $disabled = "$t$Suffix"
            if (Test-Path $t) { Write-Host "ENABLED  $t" }
            elseif (Test-Path $disabled) { Write-Host "disabled $t" }
            else { Write-Host "missing  $t" }
        }
    }
}
