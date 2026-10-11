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

$base = "https://github.com/$repo/releases"
Write-Host 'Looking up the release...'
if ($Version) {
    $tag = 'v' + $Version.TrimStart('v')
} else {
    # The latest release's page redirects to .../releases/tag/vX.Y.Z: no API, no rate limit.
    try {
        $page = Invoke-WebRequest -Uri "$base/latest" -Method Head -UseBasicParsing
    } catch {
        throw "Cannot reach $base ($($_.Exception.Message))"
    }
    $final = if ($page.BaseResponse.ResponseUri) { $page.BaseResponse.ResponseUri } # Windows PowerShell
             else { $page.BaseResponse.RequestMessage.RequestUri } # PowerShell 7
    $tag = $final.AbsoluteUri.TrimEnd('/').Split('/')[-1]
    if ($tag -notmatch '^v\d') { throw "No published release found at $base" }
}
$name = "ClipItTo-$($tag.TrimStart('v'))-setup.exe"

# The checksum: from the release's SHA256SUMS, else (older releases) from the API.
$sha = $null
try {
    $content = (Invoke-WebRequest -Uri "$base/download/$tag/SHA256SUMS" -UseBasicParsing).Content
    if ($content -is [byte[]]) { $content = [Text.Encoding]::UTF8.GetString($content) }
    foreach ($line in ($content -split "`r?`n")) {
        if ($line -match '^([0-9a-f]{64}) [ *](.+)$' -and $Matches[2] -eq $name) { $sha = $Matches[1] }
    }
} catch {}
if (-not $sha) {
    try {
        $release = Invoke-RestMethod -Uri "https://api.github.com/repos/$repo/releases/tags/$tag" -Headers @{ Accept = 'application/vnd.github+json' }
        $digest = ($release.assets | Where-Object { $_.name -eq $name }).digest
        if ($digest -match '^sha256:([0-9a-f]{64})$') { $sha = $Matches[1] }
    } catch {}
}

$dir = if ($DownloadOnly) { $DownloadOnly } else { $env:TEMP }
New-Item -ItemType Directory -Force -Path $dir | Out-Null
$file = Join-Path $dir $name

Write-Host "Downloading $name..."
try {
    Invoke-WebRequest -Uri "$base/download/$tag/$name" -OutFile $file -UseBasicParsing
} catch {
    throw "Cannot download $base/download/$tag/$name (is $tag a release with a Windows installer?)"
}
if ($sha) {
    if ((Get-FileHash -Algorithm SHA256 -Path $file).Hash -ne $sha) {
        Remove-Item $file
        throw 'Checksum mismatch: the download is damaged.'
    }
    Write-Host 'Checksum OK.'
} else {
    Write-Host "Note: no checksum available for $name (GitHub's API may be rate-limited); not checked."
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
