param(
    [string]$Game = 'none',
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [string]$Python = 'python3',
    [string]$VcpkgRoot = $env:VCPKG_ROOT,
    [switch]$Build
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
Push-Location $repo
try {
    function Invoke-Checked([string]$Program, [string[]]$Arguments) {
        & $Program @Arguments
        if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
    }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (!(Test-Path $vswhere)) { throw 'Install Visual Studio 2022 or newer with Desktop development with C++.' }
    $vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'Visual Studio C++ x64 tools were not found.' }
    if (!$VcpkgRoot) {
        $VcpkgRoot = Join-Path $vs 'VC\vcpkg'
        if (!(Test-Path "$VcpkgRoot\vcpkg.exe")) {
            $VcpkgRoot = Join-Path $repo 'build\deps\vcpkg'
            if (!(Test-Path "$VcpkgRoot\.git")) {
                Invoke-Checked git @('clone', 'https://github.com/microsoft/vcpkg.git', $VcpkgRoot)
            }
            Invoke-Checked "$VcpkgRoot\bootstrap-vcpkg.bat" @('-disableMetrics')
        }
    }
    $env:VCPKG_ROOT = (Resolve-Path $VcpkgRoot).Path
    $env:VCPKG_DISABLE_METRICS = '1'
    Invoke-Checked $Python @('-c', 'import sys; assert sys.version_info >= (3, 11), "Python 3.11+ is required"')
    if (!(Test-Path '.venv\Scripts\python.exe')) {
        Invoke-Checked $Python @('-m', 'venv', '.venv')
    }
    Invoke-Checked '.venv\Scripts\python.exe' @('-m', 'pip', 'install', '-r', 'requirements.txt')
    Invoke-Checked "$env:VCPKG_ROOT\vcpkg.exe" @('install', '--triplet', 'x64-windows',
        '--x-install-root=build/vcpkg_installed', '--downloads-root=build/deps/downloads',
        '--x-buildtrees-root=build/deps/buildtrees', '--x-packages-root=build/deps/packages', '--binarysource=clear')
    Invoke-Checked '.venv\Scripts\python.exe' @('tools/msvc_solution.py', '--root', $repo, '--game', $Game)
    if ($Build) {
        & (Join-Path $PSScriptRoot 'build_msvc_project.ps1') -Game $Game -Configuration $Configuration
    }
    Write-Host "Open $repo\v64recomp.sln in Visual Studio."
} finally {
    Pop-Location
}
