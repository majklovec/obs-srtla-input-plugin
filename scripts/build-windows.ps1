<#
.SYNOPSIS
    Builds the OBS SRTLA Source plugin for Windows x64 and, optionally, its
    NSIS installer or installs it straight into OBS Studio.

.DESCRIPTION
    Checks the build prerequisites (Visual Studio 2022 C++ tools, CMake 3.28+,
    Git, and NSIS for the installer), then configures and builds the plugin
    with the `windows-x64` CMake preset. The first configure downloads the
    pre-built OBS dependencies and the OBS Studio sources listed in
    buildspec.json into .deps\ and builds libobs there (network access and
    a few minutes are needed once).

    Outputs:
      build_x64\Release\obs-srtla-source.dll
      release\obs-srtla-source-<version>-windows-x64-installer.exe  (-Installer)

.PARAMETER Configuration
    CMake configuration to build: Release (default), RelWithDebInfo, Debug or MinSizeRel.

.PARAMETER Installer
    Also build the NSIS installer (requires makensis.exe from NSIS 3).

.PARAMETER InstallToObs
    Copy the built plugin into the OBS Studio installation directory found in
    the registry (or -ObsDir). Needs an elevated prompt for Program Files and
    OBS must be closed.

.PARAMETER ObsDir
    OBS Studio installation directory for -InstallToObs (the folder containing
    bin\64bit\obs64.exe).

.PARAMETER Clean
    Remove build_x64 before configuring.

.EXAMPLE
    .\scripts\build-windows.ps1 -Installer
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'RelWithDebInfo', 'Debug', 'MinSizeRel')]
    [string] $Configuration = 'Release',
    [switch] $Installer,
    [switch] $InstallToObs,
    [string] $ObsDir,
    [switch] $Clean
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

$ProjectRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$BuildDir = Join-Path $ProjectRoot 'build_x64'
$Preset = 'windows-x64'
$pluginDll = $null
$installerExe = $null

function Write-Step([string] $Message) {
    Write-Host ''
    Write-Host "==> $Message" -ForegroundColor Cyan
}

function Fail([string] $Message) {
    Write-Host "Error: $Message" -ForegroundColor Red
    exit 1
}

function Invoke-Checked([string] $Executable, [string[]] $Arguments) {
    Write-Host "> $Executable $($Arguments -join ' ')" -ForegroundColor DarkGray
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) {
        Fail "'$Executable' exited with code $LASTEXITCODE"
    }
}

function Find-Cmake {
    $cmake = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmake) {
        return $cmake.Source
    }
    # Visual Studio bundles CMake; fall back to it when none is on PATH.
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $vsPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($vsPath) {
            $candidate = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
            if (Test-Path -LiteralPath $candidate) {
                return $candidate
            }
        }
    }
    return $null
}

function Find-Makensis {
    $makensis = Get-Command makensis -ErrorAction SilentlyContinue
    if ($makensis) {
        return $makensis.Source
    }
    foreach ($dir in @(
            (Join-Path ${env:ProgramFiles(x86)} 'NSIS'),
            (Join-Path $env:ProgramFiles 'NSIS'),
            (Join-Path $env:LOCALAPPDATA 'Programs\NSIS'))) {
        $candidate = Join-Path $dir 'makensis.exe'
        if (Test-Path -LiteralPath $candidate) {
            return $candidate
        }
    }
    return $null
}

function Find-ObsDir {
    foreach ($key in @('HKLM:\SOFTWARE\OBS Studio', 'HKCU:\SOFTWARE\OBS Studio')) {
        if (Test-Path -LiteralPath $key) {
            $value = (Get-ItemProperty -LiteralPath $key).'(default)'
            if ($value -and (Test-Path -LiteralPath (Join-Path $value 'bin\64bit\obs64.exe'))) {
                return $value
            }
        }
    }
    $default = Join-Path $env:ProgramFiles 'obs-studio'
    if (Test-Path -LiteralPath (Join-Path $default 'bin\64bit\obs64.exe')) {
        return $default
    }
    return $null
}

# --- Prerequisites -----------------------------------------------------------
Write-Step 'Checking prerequisites'

if (-not [System.Environment]::Is64BitOperatingSystem) {
    Fail 'A 64-bit Windows system is required.'
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    Fail 'Visual Studio 2022 was not found. Install it (or the Build Tools) with the "Desktop development with C++" workload.'
}
$vsInstall = & $vswhere -latest -products * -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsInstall) {
    Fail 'Visual Studio 2022 with the C++ build tools (Microsoft.VisualStudio.Component.VC.Tools.x86.x64) was not found.'
}
Write-Host "Visual Studio: $vsInstall"

$cmakeExe = Find-Cmake
if (-not $cmakeExe) {
    Fail 'CMake was not found. Install CMake 3.28 or newer (https://cmake.org/download/ or `winget install Kitware.CMake`).'
}
$cmakeVersionText = (& $cmakeExe --version | Select-Object -First 1) -replace '^cmake version ', ''
$cmakeVersion = [Version] (($cmakeVersionText -split '-')[0])
if ($cmakeVersion -lt [Version] '3.28.0') {
    Fail "CMake 3.28 or newer is required, found $cmakeVersionText."
}
Write-Host "CMake $cmakeVersionText`: $cmakeExe"

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Fail 'Git was not found. It is needed to fetch the embedded SRTLA, spdlog and wepoll sources (`winget install Git.Git`).'
}

if ($Installer) {
    $makensisExe = Find-Makensis
    if (-not $makensisExe) {
        Fail 'makensis.exe was not found. Install NSIS 3 (https://nsis.sourceforge.io or `winget install NSIS.NSIS`) or omit -Installer.'
    }
    Write-Host "NSIS: $makensisExe"
    # Let CMake's find_program pick up the same executable.
    $env:PATH = (Split-Path -Parent $makensisExe) + ';' + $env:PATH
}

# --- Configure & build -------------------------------------------------------
Push-Location $ProjectRoot
try {
    if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
        Write-Step 'Removing previous build directory'
        Remove-Item -LiteralPath $BuildDir -Recurse -Force
    }

    Write-Step 'Configuring (first run downloads OBS dependencies and builds libobs into .deps)'
    Invoke-Checked $cmakeExe @('--preset', $Preset)

    Write-Step "Building ($Configuration)"
    Invoke-Checked $cmakeExe @('--build', '--preset', $Preset, '--config', $Configuration, '--parallel')

    $pluginDll = Join-Path $BuildDir "$Configuration\obs-srtla-source.dll"
    if (-not (Test-Path -LiteralPath $pluginDll)) {
        Fail "Build finished without producing $pluginDll"
    }

    if ($Installer) {
        Write-Step 'Building the NSIS installer'
        Invoke-Checked $cmakeExe @('--build', $BuildDir, '--config', $Configuration, '--target', 'installer')
        $installerExe = Get-ChildItem -LiteralPath (Join-Path $ProjectRoot 'release') -Filter 'obs-srtla-source-*-windows-x64-installer.exe' |
            Sort-Object LastWriteTime -Descending | Select-Object -First 1
    }

    if ($InstallToObs) {
        Write-Step 'Installing into OBS Studio'
        if (-not $ObsDir) {
            $ObsDir = Find-ObsDir
        }
        if (-not $ObsDir -or -not (Test-Path -LiteralPath (Join-Path $ObsDir 'bin\64bit\obs64.exe'))) {
            Fail 'OBS Studio installation not found; pass -ObsDir <folder containing bin\64bit\obs64.exe>.'
        }
        if (Get-Process -Name 'obs64' -ErrorAction SilentlyContinue) {
            Fail 'OBS Studio is running. Close it before installing the plugin.'
        }
        Invoke-Checked $cmakeExe @('--install', $BuildDir, '--config', $Configuration, '--prefix', $ObsDir)
        Write-Host "Installed into $ObsDir (obs-plugins\64bit and data\obs-plugins\obs-srtla-source)."
    }
}
finally {
    Pop-Location
}

Write-Step 'Done'
Write-Host "Plugin:    $pluginDll"
if ($Installer -and $installerExe) {
    Write-Host "Installer: $($installerExe.FullName)"
}
Write-Host 'The embedded irlserver/srtla code is AGPL-3.0; see README.md (License).'
