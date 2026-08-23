#Requires -Version 5.1
$ErrorActionPreference = "Stop"

$projectDir = Split-Path -Parent $PSScriptRoot
$modName = "RVThereYetHeadTracking"

Import-Module (Join-Path $projectDir "cameraunlock-core/powershell/ReleaseWorkflow.psm1") -Force

$dxgiProxy = Join-Path $projectDir "build/Release/dxgi.dll"
if (-not (Test-Path $dxgiProxy)) {
    throw "Release build not found at $dxgiProxy. Run 'pixi run build' first."
}

$version = (Select-String -Path (Join-Path $projectDir "pixi.toml") -Pattern '^version\s*=\s*"([^"]+)"').Matches[0].Groups[1].Value

$releaseDir = Join-Path $projectDir "release"
$staging = Join-Path $releaseDir "staging-installer"
$nexusStaging = Join-Path $releaseDir "staging-nexus"

if (Test-Path $releaseDir) { Remove-Item $releaseDir -Recurse -Force }
New-Item -ItemType Directory -Path $releaseDir | Out-Null
New-Item -ItemType Directory -Path $staging | Out-Null
New-Item -ItemType Directory -Path $nexusStaging | Out-Null

# Installer ZIP: install.cmd, uninstall.cmd, plugins/, docs.
# No vendor/ - the dxgi proxy is built from source, and dxgi_orig.dll is
# copied from the user's own System32 by install.cmd.
Copy-Item (Join-Path $projectDir "scripts/install.cmd") -Destination $staging
Copy-Item (Join-Path $projectDir "scripts/install.ps1") -Destination $staging
Copy-Item (Join-Path $projectDir "scripts/uninstall.cmd") -Destination $staging
Copy-Item (Join-Path $projectDir "scripts/uninstall.ps1") -Destination $staging
New-Item -ItemType Directory -Path (Join-Path $staging "plugins") | Out-Null
Copy-Item $dxgiProxy -Destination (Join-Path $staging "plugins")

$ini = Join-Path $projectDir "HeadTracking.ini"
if (Test-Path $ini) { Copy-Item $ini -Destination (Join-Path $staging "plugins") }

# shared/ bundle for install.cmd/uninstall.cmd: find-game.ps1 +
# GamePathDetection.psm1 + games.json + optional script bodies.
# -NoRefresh because release.ps1 owns submodule sync; local repackages
# shouldn't mutate cameraunlock-core as a side effect.
Copy-SharedBundle -StagingDir $staging -NoRefresh

# Launcher manifest: the contract Lopari ingests. The launcher reads exactly
# this filename (deploy/manifest.rs: MANIFEST_FILE = "launcher-manifest.json")
# from the installer ZIP root. delivery_mode is "install_cmd", so the launcher
# shells out to install.cmd, which detects the store (Win64 or WinGDK) and
# deploys to whichever build(s) are present.
$manifest = Join-Path $projectDir "launcher-manifest.json"
if (-not (Test-Path $manifest)) {
    throw "launcher-manifest.json not found at project root. The launcher manifest must ship in the installer ZIP."
}
Copy-Item $manifest -Destination $staging

# The installer ZIP is a binary distribution: MinHook's BSD-2-Clause and the
# MIT of cameraunlock-core both require their notices to travel with the
# payload, so a missing notice file fails the build rather than shipping a
# ZIP that quietly drops an attribution.
foreach ($doc in "README.md","LICENSE","CHANGELOG.md","THIRD-PARTY-NOTICES.md") {
    $p = Join-Path $projectDir $doc
    if (-not (Test-Path $p)) {
        throw "Required document not found: $doc. Every published ZIP is a binary distribution and must carry it."
    }
    Copy-Item $p -Destination $staging
}

$installerZip = Join-Path $releaseDir "RVThereYetHeadTracking-v$version-installer.zip"
Compress-Archive -Path (Join-Path $staging "*") -DestinationPath $installerZip -Force
Write-Host "Wrote $installerZip" -ForegroundColor Green

# Nexus ZIP: ships both deploy-path subtrees so a single extract at the game's
# package root populates whichever build the user has. Steam goes under
# Ride\Binaries\Win64\; Game Pass / Xbox goes under Ride\Binaries\WinGDK\. The
# unused folder just stays unused on the other platform - the WinGDK exe never
# loads the Win64 dxgi.dll and vice versa. Nexus users still copy
# %SystemRoot%\System32\dxgi.dll to dxgi_orig.dll themselves; documented in the
# bundled README.
foreach ($subdir in "Ride\Binaries\Win64", "Ride\Binaries\WinGDK") {
    $deployDir = Join-Path $nexusStaging $subdir
    New-Item -ItemType Directory -Path $deployDir -Force | Out-Null
    Copy-Item $dxgiProxy -Destination $deployDir
    if (Test-Path $ini) { Copy-Item $ini -Destination $deployDir }
}
$nexusZip = Join-Path $releaseDir "RVThereYetHeadTracking-v$version-nexus.zip"
# The Nexus ZIP is a binary distribution too: the licences of everything
# compiled into or bundled with the payload require their notices to travel
# with it, so LICENSE and THIRD-PARTY-NOTICES.md ship at its root.
foreach ($noticeDoc in @('LICENSE', 'THIRD-PARTY-NOTICES.md')) {
    $noticeSrc = Join-Path $projectDir $noticeDoc
    if (-not (Test-Path $noticeSrc)) {
        throw "Required notice file not found: $noticeDoc. Every published ZIP is a binary distribution and must carry it."
    }
    Copy-Item $noticeSrc -Destination $nexusStaging -Force
    Write-Host "  $noticeDoc" -ForegroundColor Green
}

# Nexus users extract into the game folder, so they get the Nexus README, not
# the installer one. Copied last: the notice loop above must not overwrite it.
$nexusReadme = Join-Path $projectDir "scripts/nexus-readme.md"
if (-not (Test-Path $nexusReadme)) {
    throw "scripts/nexus-readme.md not found. The Nexus ZIP must ship its own README."
}
Copy-Item $nexusReadme -Destination (Join-Path $nexusStaging "README.md") -Force
Compress-Archive -Path (Join-Path $nexusStaging "*") -DestinationPath $nexusZip -Force
Write-Host "Wrote $nexusZip" -ForegroundColor Green
