#Requires -Version 5.1
# Regression tests for scripts/uninstall.ps1.
#
# Run with:  Invoke-Pester -Path tests/Uninstall.Tests.ps1
#
# Focus: the restore path must NOT delete a file it just restored from .backup.
# Before the fix, uninstall restored the user's pre-mod dxgi.dll/HeadTracking.ini
# from .backup and then immediately deleted it (the "does a .backup still exist?"
# guard was evaluated after the backup had already been consumed), destroying the
# user's original file.

$repoRoot     = Split-Path -Parent $PSScriptRoot
$uninstallPs1 = Join-Path $repoRoot 'scripts/uninstall.ps1'
$steamRelpath = 'Ride\Binaries\Win64\Ride-Win64-Shipping.exe'

function New-FakeInstall {
    param([string]$Root)
    $exeDir = Join-Path $Root 'Ride\Binaries\Win64'
    New-Item -ItemType Directory -Path $exeDir -Force | Out-Null
    Set-Content -Path (Join-Path $exeDir 'Ride-Win64-Shipping.exe') -Value 'fake-exe' -NoNewline
    return $exeDir
}

function Invoke-Uninstall {
    param([string]$GivenPath, [switch]$Force)
    $args = @('-NoProfile','-ExecutionPolicy','Bypass','-File',$uninstallPs1,'-GivenPath',$GivenPath,'-Yes')
    if ($Force) { $args += '-Force' }
    & powershell @args | Out-Null
}

Describe 'uninstall.ps1 restore-from-backup' {

    It 'restores the pre-mod dxgi.dll and does not delete it' {
        $root = Join-Path $env:TEMP ("rvty-uninst-{0}" -f ([guid]::NewGuid()))
        try {
            $exeDir = New-FakeInstall -Root $root
            # Simulate a prior install that backed up the user's own dxgi.dll.
            Set-Content -Path (Join-Path $exeDir 'dxgi.dll')        -Value 'our-proxy'    -NoNewline
            Set-Content -Path (Join-Path $exeDir 'dxgi.dll.backup') -Value 'user-original' -NoNewline
            Set-Content -Path (Join-Path $exeDir 'dxgi_orig.dll')   -Value 'system-copy'  -NoNewline

            Invoke-Uninstall -GivenPath $root

            $dxgi = Join-Path $exeDir 'dxgi.dll'
            (Test-Path $dxgi) | Should Be $true
            (Get-Content -Raw $dxgi).TrimEnd() | Should Be 'user-original'
            (Test-Path (Join-Path $exeDir 'dxgi.dll.backup')) | Should Be $false
            # The captured System32 copy has no backup, so it is removed.
            (Test-Path (Join-Path $exeDir 'dxgi_orig.dll')) | Should Be $false
        } finally {
            Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    It 'removes our proxy dxgi.dll when there is no backup' {
        $root = Join-Path $env:TEMP ("rvty-uninst-{0}" -f ([guid]::NewGuid()))
        try {
            $exeDir = New-FakeInstall -Root $root
            Set-Content -Path (Join-Path $exeDir 'dxgi.dll')      -Value 'our-proxy'   -NoNewline
            Set-Content -Path (Join-Path $exeDir 'dxgi_orig.dll') -Value 'system-copy' -NoNewline

            Invoke-Uninstall -GivenPath $root

            (Test-Path (Join-Path $exeDir 'dxgi.dll'))      | Should Be $false
            (Test-Path (Join-Path $exeDir 'dxgi_orig.dll')) | Should Be $false
        } finally {
            Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    It 'leaves generic legacy loader DLLs intact without -Force (may belong to other mods)' {
        $root = Join-Path $env:TEMP ("rvty-uninst-{0}" -f ([guid]::NewGuid()))
        try {
            $exeDir = New-FakeInstall -Root $root
            Set-Content -Path (Join-Path $exeDir 'dxgi.dll')     -Value 'our-proxy'      -NoNewline
            Set-Content -Path (Join-Path $exeDir 'winmm.dll')    -Value 'asi-loader'     -NoNewline
            Set-Content -Path (Join-Path $exeDir 'dinput8.dll')  -Value 'other-mod'      -NoNewline
            Set-Content -Path (Join-Path $exeDir 'xinput1_3.dll') -Value 'x360ce'        -NoNewline
            Set-Content -Path (Join-Path $exeDir 'RVThereYetHeadTracking.asi') -Value 'ours' -NoNewline

            Invoke-Uninstall -GivenPath $root

            # Ambiguous filenames stay; unambiguously-ours legacy file goes.
            (Test-Path (Join-Path $exeDir 'winmm.dll'))     | Should Be $true
            (Test-Path (Join-Path $exeDir 'dinput8.dll'))   | Should Be $true
            (Test-Path (Join-Path $exeDir 'xinput1_3.dll')) | Should Be $true
            (Test-Path (Join-Path $exeDir 'RVThereYetHeadTracking.asi')) | Should Be $false
            (Test-Path (Join-Path $exeDir 'dxgi.dll'))      | Should Be $false
        } finally {
            Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    It 'removes generic legacy loader DLLs with -Force' {
        $root = Join-Path $env:TEMP ("rvty-uninst-{0}" -f ([guid]::NewGuid()))
        try {
            $exeDir = New-FakeInstall -Root $root
            Set-Content -Path (Join-Path $exeDir 'winmm.dll')    -Value 'old-dev-proxy' -NoNewline
            Set-Content -Path (Join-Path $exeDir 'dinput8.dll')  -Value 'old-dev-proxy' -NoNewline
            Set-Content -Path (Join-Path $exeDir 'xinput1_3.dll') -Value 'old-dev-proxy' -NoNewline

            Invoke-Uninstall -GivenPath $root -Force

            (Test-Path (Join-Path $exeDir 'winmm.dll'))     | Should Be $false
            (Test-Path (Join-Path $exeDir 'dinput8.dll'))   | Should Be $false
            (Test-Path (Join-Path $exeDir 'xinput1_3.dll')) | Should Be $false
        } finally {
            Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    It 'with -Force removes both proxy and backup (no restore)' {
        $root = Join-Path $env:TEMP ("rvty-uninst-{0}" -f ([guid]::NewGuid()))
        try {
            $exeDir = New-FakeInstall -Root $root
            Set-Content -Path (Join-Path $exeDir 'dxgi.dll')        -Value 'our-proxy'     -NoNewline
            Set-Content -Path (Join-Path $exeDir 'dxgi.dll.backup') -Value 'user-original' -NoNewline

            Invoke-Uninstall -GivenPath $root -Force

            (Test-Path (Join-Path $exeDir 'dxgi.dll'))        | Should Be $false
            (Test-Path (Join-Path $exeDir 'dxgi.dll.backup')) | Should Be $false
        } finally {
            Remove-Item $root -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}
