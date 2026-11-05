# The Windows part of scripts/make-release.sh: build, tests, a smoke run of the staged app, and the installer.
#
#   powershell -ExecutionPolicy Bypass -File scripts\make-release-windows.ps1 -Version 0.11.0 -OutputDir C:\out [-NoTests]
param(
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][string]$OutputDir,
    [switch]$NoTests
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$extra = @()
if ($NoTests) { $extra += '-NoTests' }
& (Join-Path $PSScriptRoot 'build-windows.ps1') -Stage @extra
if ($LASTEXITCODE) { throw 'build-windows.ps1 failed' }

$stage = Join-Path $root 'build\windows-stage'

# Smoke test the staged app, as CI does.
$env:QT_QPA_PLATFORM = 'offscreen'
$env:CLIPITTO_SERVER_NAME = 'clipitto-release'
$daemon = Start-Process "$stage\bin\clipitto-daemon.exe" -PassThru
Start-Sleep -Seconds 3
& "$stage\bin\clipitto.exe" --ingest 'hello from the release build'
$status = & "$stage\bin\clipitto.exe" --status
$status
& "$stage\bin\clipitto.exe" --quit
if (-not $daemon.WaitForExit(10000)) { $daemon.Kill(); throw 'the daemon did not quit' }
if (-not ($status -match '^items: 1$')) { throw "unexpected status: $status" }
Remove-Item Env:QT_QPA_PLATFORM, Env:CLIPITTO_SERVER_NAME

$iscc = Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'
if (-not (Test-Path $iscc)) { $iscc = Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe' }
if (-not (Test-Path $iscc)) { throw 'Inno Setup not found (winget install JRSoftware.InnoSetup).' }
New-Item -ItemType Directory -Force $OutputDir | Out-Null
& $iscc "/DStageDir=$stage" "/DOutputDir=$OutputDir" "/DAppVersion=$Version" (Join-Path $root 'packaging\windows\clipitto.iss')
if ($LASTEXITCODE) { throw 'iscc failed' }
Get-ChildItem $OutputDir -Filter '*-setup.exe' | ForEach-Object { Write-Host "Installer: $($_.FullName)" }
