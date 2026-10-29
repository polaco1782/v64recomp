param(
    [Parameter(Mandatory = $true)][string]$Game,
    [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
    [ValidateSet('Build', 'Rebuild', 'Clean')][string]$Action = 'Build'
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
if ($Game -notmatch '^[a-z0-9][a-z0-9-]*$') { throw "Invalid game ID: $Game" }
$key = if ($Game -eq 'none') { 'recomp' } else { $Game }
if ($Game -ne 'none' -and !(Test-Path (Join-Path $repo "games\$Game\CMakeLists.txt"))) {
    throw "Unknown game: $Game"
}
$buildDirectory = Join-Path $repo "build\msvc\$key\$($Configuration.ToLowerInvariant())"
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed with exit code $LASTEXITCODE" }
}
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio C++ x64 tools were not found.' }
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (!(Test-Path $cmake)) { $cmake = (Get-Command cmake -ErrorAction Stop).Source }
if ($Action -eq 'Clean') {
    if (Test-Path "$buildDirectory\CMakeCache.txt") {
        Invoke-Checked $cmake @('--build', $buildDirectory, '--config', $Configuration, '--target', 'clean')
    }
    exit 0
}
if (!$env:VCPKG_ROOT) {
    $env:VCPKG_ROOT = Join-Path $vs 'VC\vcpkg'
    if (!(Test-Path "$env:VCPKG_ROOT\vcpkg.exe")) {
        $env:VCPKG_ROOT = Join-Path $repo 'build\deps\vcpkg'
    }
}
if (!(Test-Path "$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake") -or
    !(Test-Path "$repo\build\vcpkg_installed\x64-windows\share\PThreads4W\PThreads4WConfig.cmake")) {
    throw 'Run .\tools\setup_windows.ps1 first to install the Windows dependencies.'
}
$major = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationVersion).Split('.')[0]
$generator = switch ($major) {
    '18' { 'Visual Studio 18 2026' }
    '17' { 'Visual Studio 17 2022' }
    default { throw 'Visual Studio 2022 or newer is required.' }
}
Invoke-Checked $cmake @('-S', $repo, '-B', $buildDirectory, '-G', $generator, '-A', 'x64',
    "-DCMAKE_BUILD_TYPE=$Configuration", "-DRECOMP_GAME=$Game", '-DRECOMP_BUILD_RT64=ON',
    '-DRECOMP_BUILD_AUDIO=ON', '-DRECOMP_EMBED_ROM=OFF',
    "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake",
    '-DVCPKG_TARGET_TRIPLET=x64-windows', "-DVCPKG_INSTALLED_DIR=$repo/build/vcpkg_installed",
    '-DVCPKG_MANIFEST_INSTALL=OFF')
$targets = if ($Game -eq 'none') {
    @('recomp_runtime', 'recomp_jpeg', 'recomp_audio', 'recomp_renderer')
} else { @($Game) }
$buildArguments = @('--build', $buildDirectory, '--config', $Configuration, '--parallel', '4', '--target') + $targets
if ($Action -eq 'Rebuild') { $buildArguments += '--clean-first' }
Invoke-Checked $cmake $buildArguments
