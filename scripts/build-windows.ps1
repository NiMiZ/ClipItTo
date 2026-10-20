# Builds (and tests) ClipItTo for Windows on this machine, the way CI does.
# Needs: Visual Studio or Build Tools with "Desktop development with C++" (MSVC, CMake,
# Ninja), Qt 6.5+ for MSVC, and vcpkg.
#
#   powershell -ExecutionPolicy Bypass -File scripts\build-windows.ps1 [-NoTests] [-Stage]
#     -QtDir   Qt's MSVC folder (default: $env:QT_ROOT_DIR, else D:\Qt or C:\Qt\6.5.3\msvc2019_64)
#     -Vcpkg   vcpkg checkout   (default: $env:VCPKG_ROOT, else D:\vcpkg or C:\vcpkg)
#     -Stage   also install a runnable copy into build\windows-stage (windeployqt included)
param(
    [string]$QtDir = $env:QT_ROOT_DIR,
    [string]$Vcpkg = $env:VCPKG_ROOT,
    [switch]$NoTests,
    [switch]$Stage
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function First-Existing([string[]]$paths) {
    foreach ($p in $paths) { if ($p -and (Test-Path $p)) { return (Resolve-Path $p).Path } }
    return $null
}

$QtDir = First-Existing @($QtDir, 'D:\Qt\6.5.3\msvc2019_64', 'C:\Qt\6.5.3\msvc2019_64')
if (-not $QtDir) { throw 'Qt for MSVC not found: pass -QtDir or set QT_ROOT_DIR.' }
$Vcpkg = First-Existing @($Vcpkg, 'D:\vcpkg', 'C:\vcpkg')
if (-not $Vcpkg) { throw 'vcpkg not found: pass -Vcpkg or set VCPKG_ROOT.' }

# The MSVC environment (cl, link, cmake, ninja on PATH), as a Developer prompt has it.
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio / Build Tools with the C++ workload (MSVC) found.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
foreach ($line in (cmd /c "`"$vcvars`" >nul && set")) {
    if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2] }
}
$env:QT_ROOT_DIR = $QtDir
$env:VCPKG_ROOT = $Vcpkg
$env:PATH = "$QtDir\bin;$env:PATH" # Qt's DLLs, for running the tests
Write-Host "MSVC: $vs"
Write-Host "Qt:   $QtDir"
Write-Host "vcpkg: $Vcpkg"

Push-Location $root
try {
    # Visual Studio updates move the compiler: a build folder still pointing at the old one
    # is configured again from scratch.
    $fresh = @()
    $cache = Join-Path $root 'build\windows\CMakeCache.txt'
    if (Test-Path $cache) {
        $compiler = (Select-String -Path $cache -Pattern '^CMAKE_CXX_COMPILER:\w+=(.+)$').Matches | Select-Object -First 1
        if ($compiler -and -not (Test-Path $compiler.Groups[1].Value)) {
            Write-Host 'The compiler moved (Visual Studio was updated): configuring from scratch.'
            $fresh = @('--fresh')
        }
    }
    cmake --preset windows @fresh
    if ($LASTEXITCODE) { throw 'configure failed' }
    cmake --build --preset windows
    if ($LASTEXITCODE) { throw 'build failed' }
    if (-not $NoTests) {
        ctest --preset windows
        if ($LASTEXITCODE) { throw 'tests failed' }
    }
    if ($Stage) {
        $stageDir = Join-Path $root 'build\windows-stage'
        cmake --install build\windows --prefix $stageDir
        if ($LASTEXITCODE) { throw 'install failed' }
        Write-Host "Runnable copy: $stageDir\bin\clipitto-daemon.exe"
    }
} finally {
    Pop-Location
}
