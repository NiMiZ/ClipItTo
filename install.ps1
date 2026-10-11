# Installs ClipItTo on Windows 10/11 (x64) from the GitHub release: downloads the installer,
# checks its SHA-256 against the release, installs it for the current user (no admin rights),
# with "Start ClipItTo when I sign in", and starts it.
#
#   irm https://github.com/NiMiZ/ClipItTo/releases/latest/download/install.ps1 | iex
#   powershell -ExecutionPolicy Bypass -File install.ps1 [-Version 0.1.0] [-DownloadOnly DIR]
param(
    [string]$Version,
    [string]$DownloadOnly
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue' # Invoke-WebRequest is very slow with a progress bar
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$repo = 'NiMiZ/ClipItTo'
$appId = '{8C4D2B7E-5A61-4F0B-9E3C-2D7F1A6B9C40}_is1' # AppId in packaging/windows/clipitto.iss

if (-not [Environment]::Is64BitOperatingSystem) { throw 'ClipItTo needs 64-bit Windows.' }

if ($Version) {
    $api = "https://api.github.com/repos/$repo/releases/tags/v$($Version.TrimStart('v'))"
} else {
    $api = "https://api.github.com/repos/$repo/releases/latest"
}
Write-Host 'Looking up the release...'
try {
    $release = Invoke-RestMethod -Uri $api -Headers @{ Accept = 'application/vnd.github+json' }
} catch {
    throw "Cannot read $api (no such release, or GitHub is unreachable): $($_.Exception.Message)"
}
$asset = $release.assets | Where-Object { $_.name -like 'ClipItTo-*-setup.exe' } | Select-Object -First 1
if (-not $asset) { throw "Release $($release.tag_name) has no Windows installer." }

$dir = if ($DownloadOnly) { $DownloadOnly } else { $env:TEMP }
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$file = Join-Path $dir $asset.name

Write-Host "Downloading $($asset.name)..."
Invoke-WebRequest -Uri $asset.browser_download_url -OutFile $file -UseBasicParsing
if ($asset.digest -match '^sha256:([0-9a-f]{64})$') {
    $actual = (Get-FileHash -Algorithm SHA256 -Path $file).Hash
    if ($actual -ne $Matches[1]) {
        Remove-Item $file
        throw 'Checksum mismatch: the download is damaged.'
    }
    Write-Host 'Checksum OK.'
} else {
    Write-Host "Note: the release lists no checksum for $($asset.name); skipping the check."
}

if ($DownloadOnly) {
    Write-Host "Saved $file"
    return
}

try {
    Write-Host 'Installing...'
    $setup = Start-Process -FilePath $file -Wait -PassThru -ArgumentList @(
        '/SILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/CURRENTUSER', '/TASKS=autostart')
    if ($setup.ExitCode -ne 0) { throw "The installer failed (exit code $($setup.ExitCode))." }
} finally {
    Remove-Item $file -ErrorAction SilentlyContinue
}

# A silent install skips "Start ClipItTo now": start it here, from wherever it was installed.
$key = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\$appId"
$appDir = (Get-ItemProperty -Path $key -ErrorAction SilentlyContinue).InstallLocation
if ($appDir) {
    $daemon = Join-Path $appDir 'bin\clipitto-daemon.exe'
    if (Test-Path $daemon) { Start-Process -FilePath $daemon }
}

Write-Host ''
Write-Host 'ClipItTo is installed and running in the notification area.'
Write-Host 'Press Ctrl+Alt+V to open your clipboard history.'
Write-Host 'To remove it: Settings > Apps > Installed apps > ClipItTo > Uninstall.'
