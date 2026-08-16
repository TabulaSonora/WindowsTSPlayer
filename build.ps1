<#
.SYNOPSIS
    Builds both halves of the program, in the one order that works.

.DESCRIPTION
    This repository is built by two build systems. CMake owns the engine and src/host/; MSBuild owns
    XAML, MIDL, cppwinrt and the MSIX package. MSBuild reads a props file CMake generates, and CMake
    never invokes MSBuild, because the reverse would be a cycle neither could break.

    The failure this script exists to prevent is running only the MSBuild half. That succeeds --
    against whatever static libraries were left in build/ from last time -- and produces a program
    that links a stale engine. There is no diagnostic for it; the build is green and the sound is
    wrong. Running CMake first every time is cheap, because an unchanged tree is a no-op.

.PARAMETER Configuration
    Debug or Release. Debug maps to the CMake `dev` preset, which compiles the engine at /O2 while
    leaving our own sources at /Od -- at /Od the synth renders at about 1.4x realtime and no ring can
    absorb that.

.PARAMETER Register
    Register the built loose layout with Add-AppxPackage, which grants package identity -- and so
    file associations and the media transport controls -- without any signing.

.PARAMETER SkipCMake
    Skip the CMake half. Only correct when you have just run it yourself; see above for why this is
    not the default.
#>
[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Release')]
    [string] $Configuration = 'Release',

    [switch] $Register,
    [switch] $SkipCMake
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$preset = if ($Configuration -eq 'Debug') { 'dev' } else { 'release' }

# CMake and MSBuild both ship inside the Build Tools; neither is assumed to be on PATH, because on a
# machine with only the Build Tools installed neither is.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found. Visual Studio Build Tools 2026 with the component group`n  Microsoft.VisualStudio.ComponentGroup.WindowsAppDevelopment.VC.BuildTools`nis required."
}

$vsPath = & $vswhere -latest -products * -property installationPath
if (-not $vsPath) { throw 'No Visual Studio installation found.' }

$cmake   = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\amd64\MSBuild.exe'
if (-not (Test-Path $msbuild)) {
    $msbuild = Join-Path $vsPath 'MSBuild\Current\Bin\MSBuild.exe'
}
foreach ($tool in @($cmake, $msbuild)) {
    if (-not (Test-Path $tool)) { throw "Not found: $tool" }
}

$project = Join-Path $PSScriptRoot 'src\app\WindowsTSPlayer.vcxproj'

if (-not $SkipCMake) {
    Write-Host "==> CMake ($preset)" -ForegroundColor Cyan
    & $cmake --preset $preset
    if ($LASTEXITCODE -ne 0) { throw "cmake --preset $preset failed" }

    & $cmake --build --preset $preset
    if ($LASTEXITCODE -ne 0) { throw "cmake --build --preset $preset failed" }
}

# Restore is a separate invocation rather than -restore on the build, because the C++ project system
# resolves package .props at evaluation time: a single pass would evaluate the project before the
# packages it imports exist.
Write-Host "==> NuGet restore ($Configuration)" -ForegroundColor Cyan
& $msbuild $project -t:Restore -p:Configuration=$Configuration -p:Platform=x64 -nologo -v:minimal
if ($LASTEXITCODE -ne 0) { throw 'restore failed' }

Write-Host "==> MSBuild ($Configuration)" -ForegroundColor Cyan
& $msbuild $project -p:Configuration=$Configuration -p:Platform=x64 -nologo -v:minimal
if ($LASTEXITCODE -ne 0) { throw 'build failed' }

# The loose layout is the output directory itself, not a separate AppX\ subdirectory. That is what
# EnableMsixTooling with AppContainerApplication false gets you: AppxManifest.xml, resources.pri and
# the compiled .xbf files land beside the .exe, and Add-AppxPackage -Register takes that manifest
# directly. The AppX\ path the packaging-project flow uses does not exist here.
$layout = Join-Path $PSScriptRoot "src\app\x64\$Configuration\WindowsTSPlayer\AppxManifest.xml"

if ($Register) {
    Write-Host '==> Add-AppxPackage -Register' -ForegroundColor Cyan
    if (-not (Test-Path $layout)) { throw "Loose layout not found: $layout" }

    # Unregister first. Registering over an existing registration of the same version fails with
    # 0x80073CFB ("the package is already installed, and reinstallation was blocked"), and the
    # message's own suggestion -- increment the version -- is wrong for a development loop, where the
    # version is meaningful and the layout is rebuilt dozens of times at one version. Removing the
    # registration does not touch the files; it only drops the identity that points at them.
    $existing = Get-AppxPackage -Name 'co.losno.TabulaSonoraPlayer' -ErrorAction SilentlyContinue
    if ($existing) {
        Write-Host "    unregistering $($existing.PackageFullName)"
        Remove-AppxPackage -Package $existing.PackageFullName -ErrorAction Stop
    }

    Add-AppxPackage -Register $layout
}

Write-Host ''
Write-Host "Built $Configuration." -ForegroundColor Green
if (-not $Register -and (Test-Path $layout)) {
    Write-Host "Register it with:  Add-AppxPackage -Register $layout"
}
