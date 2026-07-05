#Requires -Version 5.1
[CmdletBinding()]
param(
    [string]$GivenPath,
    [switch]$Yes,
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# End-user uninstaller: removes the mod from every detected RV There Yet
# install (Steam + Xbox/Game Pass). Mirrors install.ps1's enumeration. Restores
# any pre-mod dxgi.dll from .backup unless -Force was passed.

$scriptDir   = $PSScriptRoot
$projectRoot = Split-Path -Parent $scriptDir

$modulePath = Join-Path $scriptDir 'shared/GamePathDetection.psm1'
if (-not (Test-Path $modulePath)) {
    $modulePath = Join-Path $projectRoot 'cameraunlock-core/powershell/GamePathDetection.psm1'
}
if (-not (Test-Path $modulePath)) {
    throw "GamePathDetection.psm1 not found. Installer ZIP is corrupt or dev checkout is incomplete."
}
Import-Module $modulePath -Force

$cfg = Get-GameConfig -GameId 'rv-there-yet'
if (-not $cfg) { throw "rv-there-yet not in games.json." }

$steamExeRelpath = $cfg.Executable
$xboxExeRelpath  = if ($cfg.ContainsKey('XboxExecutable') -and $cfg.XboxExecutable) { $cfg.XboxExecutable } else { $cfg.Executable }

function New-Install([string]$Label, [string]$Root, [string]$ExeRelpath) {
    [pscustomobject]@{ Label = $Label; Root = $Root.TrimEnd('\','/'); ExeRelpath = $ExeRelpath }
}

$installs = @()

if ($GivenPath) {
    if (-not (Test-Path -LiteralPath $GivenPath -PathType Container)) {
        throw "Given path does not exist: $GivenPath"
    }
    if (Test-Path (Join-Path $GivenPath $steamExeRelpath)) {
        $installs += New-Install 'Steam (given path)' $GivenPath $steamExeRelpath
    } elseif (Test-Path (Join-Path $GivenPath $xboxExeRelpath)) {
        $installs += New-Install 'Xbox/Game Pass (given path)' $GivenPath $xboxExeRelpath
    } else {
        # Allow uninstall against a path that no longer has an exe (game uninstalled but mod files linger).
        $installs += New-Install 'Given path' $GivenPath $steamExeRelpath
    }
} else {
    $libraries = @(Find-SteamLibraries)
    foreach ($lib in $libraries) {
        $manifest = Join-Path $lib "steamapps\appmanifest_$($cfg.SteamAppId).acf"
        if (-not (Test-Path $manifest)) { continue }
        $content = Get-Content -Raw -Path $manifest
        if ($content -match '"installdir"\s+"([^"]+)"') {
            $root = Join-Path $lib "steamapps\common\$($matches[1])"
            if (Test-Path (Join-Path $root $steamExeRelpath)) {
                $installs += New-Install 'Steam' $root $steamExeRelpath
            }
        }
    }
    if ($cfg.EnvVar) {
        $envPath = [Environment]::GetEnvironmentVariable($cfg.EnvVar)
        if ($envPath -and (Test-Path -LiteralPath $envPath -PathType Container)) {
            $alreadyTracked = $installs | Where-Object { $_.Root -ieq $envPath.TrimEnd('\','/') }
            if (-not $alreadyTracked) {
                if (Test-Path (Join-Path $envPath $steamExeRelpath)) {
                    $installs += New-Install "Env $($cfg.EnvVar) (Steam layout)" $envPath $steamExeRelpath
                } elseif (Test-Path (Join-Path $envPath $xboxExeRelpath)) {
                    $installs += New-Install "Env $($cfg.EnvVar) (WinGDK layout)" $envPath $xboxExeRelpath
                }
            }
        }
    }
    if ($cfg.ContainsKey('XboxPaths') -and $cfg.XboxPaths) {
        foreach ($xboxRoot in $cfg.XboxPaths) {
            if (Test-Path (Join-Path $xboxRoot $xboxExeRelpath)) {
                $installs += New-Install 'Xbox/Game Pass' $xboxRoot $xboxExeRelpath
            }
        }
    }
}

if ($installs.Count -eq 0) {
    Write-Host "No RV There Yet install detected. Nothing to uninstall." -ForegroundColor Yellow
    exit 0
}

$exeNames = ($installs | ForEach-Object { [System.IO.Path]::GetFileName($_.ExeRelpath) }) | Select-Object -Unique
foreach ($name in $exeNames) {
    $running = Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName + '.exe' -ieq $name }
    if ($running) {
        Write-Host "ERROR: $name is currently running. Close the game before uninstalling." -ForegroundColor Red
        exit 1
    }
}

$modFiles  = @('dxgi.dll', 'dxgi_orig.dll', 'HeadTracking.ini')
# Unambiguously ours by name; removed unconditionally.
$legacyFiles = @('RVThereYetHeadTracking.asi')
# Pre-DXGI dev builds used generic proxy names, but no release ever shipped
# them - and other mods (Ultimate ASI Loader, x360ce) legitimately own these
# filenames. We cannot attribute them, so they only come out under -Force,
# mirroring the launcher contract for unattributed loader files.
$ambiguousLegacyFiles = @('winmm.dll', 'dinput8.dll', 'xinput1_3.dll')
$logArtefacts = @('RVThereYetHeadTracking.log')

Write-Host ""
Write-Host "=== RV There Yet Head Tracking - Uninstall ===" -ForegroundColor Cyan
Write-Host ("Found {0} install(s):" -f $installs.Count)
foreach ($i in $installs) { Write-Host ("  - {0}: {1}" -f $i.Label, $i.Root) }
Write-Host ""

foreach ($install in $installs) {
    $exeDir = Split-Path -Parent (Join-Path $install.Root $install.ExeRelpath)
    Write-Host "=== $($install.Label): $exeDir ===" -ForegroundColor Cyan

    # Restore pre-mod shims from .backup unless -Force was passed. Track which
    # files we restored so the mod-file delete loop below does NOT then remove
    # the freshly restored original (the backup no longer exists at that point,
    # so a "does a .backup still exist?" check would wrongly report false).
    $restored = @{}
    foreach ($file in 'dxgi.dll','HeadTracking.ini') {
        $current = Join-Path $exeDir $file
        $backup  = "$current.backup"
        if (Test-Path $backup) {
            if ($Force) {
                Remove-Item $backup -Force
                Write-Host "  Removed $file.backup (-Force)"
            } else {
                if (Test-Path $current) { Remove-Item $current -Force }
                Move-Item $backup $current
                $restored[$file] = $true
                Write-Host "  Restored pre-mod $file from .backup"
            }
        }
    }

    foreach ($file in $modFiles) {
        # Never delete a file we just restored from .backup - that is the user's
        # pre-mod original, not our proxy.
        if ($restored.ContainsKey($file)) { continue }
        $p = Join-Path $exeDir $file
        if (Test-Path $p) {
            Remove-Item $p -Force
            Write-Host "  Removed $file"
        }
    }

    foreach ($file in $legacyFiles) {
        $p = Join-Path $exeDir $file
        if (Test-Path $p) {
            Remove-Item $p -Force
            Write-Host "  Removed $file (legacy)"
        }
    }

    foreach ($file in $ambiguousLegacyFiles) {
        $p = Join-Path $exeDir $file
        if (Test-Path $p) {
            if ($Force) {
                Remove-Item $p -Force
                Write-Host "  Removed $file (legacy, -Force)"
            } else {
                Write-Host "  Leaving $file intact (may belong to another mod). Use /force to remove."
            }
        }
    }

    foreach ($file in $logArtefacts) {
        $p = Join-Path $exeDir $file
        if (Test-Path $p) {
            Remove-Item $p -Force
            Write-Host "  Removed $file (log)"
        }
    }

    $statePath = Join-Path $install.Root '.headtracking-state.json'
    if (Test-Path $statePath) {
        Remove-Item $statePath -Force
        Write-Host "  Removed .headtracking-state.json"
    }
}

Write-Host ""
Write-Host "=== Uninstall Complete ===" -ForegroundColor Green
Write-Host ""
exit 0
