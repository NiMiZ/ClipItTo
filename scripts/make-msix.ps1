# Builds the Microsoft Store package (MSIX) from the staged Windows build, the same files the Inno Setup installer packs.
# Docs: docs/microsoft-store.md. Not yet run on a real Windows PC when this was written; read it before the first use.
#
#   powershell -ExecutionPolicy Bypass -File scripts\make-msix.ps1 [-StageDir <dir>] [-OutputDir <dir>] [-TestSign]
#
# Output: <OutputDir>\ClipItTo-<version>.msix           UNSIGNED, for the Store (Microsoft signs it after the upload).
#         with -TestSign also <OutputDir>\ClipItTo-<version>-test.msix, signed with a self-signed certificate (publisher
#         "CN=ClipItTo Test"), and ClipItTo-Test.cer to trust it, so that you can sideload it on your own PC.
#         -CleanupTest removes that certificate and the installed test package again.
#
# The package identity comes from Partner Center (Product management > Product identity). Until then these are placeholders:
#   MSIX_IDENTITY_NAME            e.g. 12345NiMiZ.ClipItTo        (Package/Identity/Name)
#   MSIX_PUBLISHER                e.g. CN=01234567-89AB-CDEF-...  (Package/Identity/Publisher)
#   MSIX_PUBLISHER_DISPLAY_NAME   e.g. NiMiZ                      (Package/Properties/PublisherDisplayName)
#   MSIX_STARTUP_ENABLED          true (default) or false: does the start-at-sign-in task start enabled
# as environment variables or as the parameters of the same name. Nothing here needs a secret.
param(
    [string]$StageDir,
    [string]$OutputDir,
    [string]$IdentityName = $env:MSIX_IDENTITY_NAME,
    [string]$Publisher = $env:MSIX_PUBLISHER,
    [string]$PublisherDisplayName = $env:MSIX_PUBLISHER_DISPLAY_NAME,
    [string]$StartupEnabled = $env:MSIX_STARTUP_ENABLED,
    [switch]$TestSign,
    [switch]$CleanupTest,
    # Refuse placeholders (use this for the package you upload to the Store).
    [switch]$RequireStoreIdentity
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$testSubject = 'CN=ClipItTo Test'
$testPackageName = 'ClipItToTest'   # the sideloaded test package's identity name: never the Store's

# Windows SDK tools: the newest folder that has the tool.
function Find-SdkTool([string]$name) {
    $bins = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $found = Get-ChildItem $bins -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^10\.' } |
        Sort-Object { [version]$_.Name } -Descending |
        ForEach-Object { Join-Path $_.FullName "x64\$name" } | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $found) { throw "$name not found: install the Windows SDK (winget install Microsoft.WindowsSDK.10.0.22621) or Visual Studio's 'Windows 10/11 SDK' component." }
    return $found
}

if ($CleanupTest) {
    # Undo -TestSign: remove the test package and its certificate (the certificate in Trusted People needs an elevated shell).
    Get-AppxPackage -Name $testPackageName | Remove-AppxPackage
    foreach ($store in 'Cert:\LocalMachine\TrustedPeople', 'Cert:\CurrentUser\My') {
        Get-ChildItem $store | Where-Object { $_.Subject -eq $testSubject } | ForEach-Object {
            Write-Host "Removing $($_.Thumbprint) from $store"
            Remove-Item $_.PSPath
        }
    }
    return
}

if (-not $StageDir) { $StageDir = Join-Path $root 'build\windows-stage' }
if (-not (Test-Path (Join-Path $StageDir 'bin\clipitto-daemon.exe'))) {
    throw "No staged build in $StageDir (needs bin\clipitto-daemon.exe). Run: scripts\build-windows.ps1 -Stage (or pass -StageDir)."
}
$StageDir = (Resolve-Path $StageDir).Path

# The version: project(... VERSION x.y.z) in CMakeLists.txt, as the installer build reads it. The Store wants the last part to be 0.
$version = (Select-String -Path (Join-Path $root 'CMakeLists.txt') -Pattern '^\s*VERSION (\d+\.\d+\.\d+)').Matches[0].Groups[1].Value
if (-not $version) { throw 'no VERSION in CMakeLists.txt' }
$msixVersion = "$version.0"
if (-not $OutputDir) { $OutputDir = Join-Path $root "dist\$version" }
New-Item -ItemType Directory -Force $OutputDir | Out-Null
$OutputDir = (Resolve-Path $OutputDir).Path

$placeholders = @()
if (-not $IdentityName) { $IdentityName = 'PLACEHOLDER-FromPartnerCenter.ClipItTo'; $placeholders += 'MSIX_IDENTITY_NAME' }
if (-not $Publisher) { $Publisher = 'CN=PLACEHOLDER-FromPartnerCenter'; $placeholders += 'MSIX_PUBLISHER' }
if (-not $PublisherDisplayName) { $PublisherDisplayName = 'PLACEHOLDER Publisher'; $placeholders += 'MSIX_PUBLISHER_DISPLAY_NAME' }
if ($StartupEnabled -notin 'true', 'false') { $StartupEnabled = 'true' }
if ($placeholders) {
    $message = "Placeholder package identity (not set: $($placeholders -join ', ')). The Store rejects this package; fill the values in from Partner Center."
    if ($RequireStoreIdentity) { throw $message }
    Write-Warning $message
}

$sdkMakeAppx = Find-SdkTool 'makeappx.exe'

function Escape-Xml([string]$s) { [Security.SecurityElement]::Escape($s) }

# One package: the staged files + Assets + the manifest, packed with MakeAppx.
function New-Msix([string]$name, [string]$identity, [string]$publisher, [string]$display, [string]$out) {
    $work = Join-Path ([IO.Path]::GetTempPath()) "clipitto-msix-$([guid]::NewGuid().ToString('N'))"
    New-Item -ItemType Directory -Force $work | Out-Null
    try {
        # The same files as the installer ("Source: StageDir\*", recursively).
        Copy-Item (Join-Path $StageDir '*') $work -Recurse -Force
        Copy-Item (Join-Path $PSScriptRoot '..\packaging\windows\msix\Assets') (Join-Path $work 'Assets') -Recurse -Force
        $manifest = Get-Content (Join-Path $root 'packaging\windows\msix\AppxManifest.xml.in') -Raw -Encoding UTF8
        $values = @{
            MSIX_IDENTITY_NAME = $identity; MSIX_PUBLISHER = $publisher; MSIX_PUBLISHER_DISPLAY_NAME = $display
            MSIX_VERSION = $msixVersion; MSIX_STARTUP_ENABLED = $StartupEnabled
        }
        foreach ($key in $values.Keys) { $manifest = $manifest.Replace("@$key@", (Escape-Xml $values[$key])) }
        if ($manifest -match '@[A-Z_]+@') { throw "unreplaced manifest variable: $($Matches[0])" }
        [IO.File]::WriteAllText((Join-Path $work 'AppxManifest.xml'), $manifest, (New-Object Text.UTF8Encoding($false)))

        # resources.pri lets the shell find the targetsize-/altform- icon variants. Made from the manifest and the Assets only
        # (not from the Qt files). Optional: without makepri the package still works with the plain file names.
        try {
            $makepri = Find-SdkTool 'makepri.exe'
            $priWork = "$work-pri"
            New-Item -ItemType Directory -Force $priWork | Out-Null
            Copy-Item (Join-Path $work 'Assets') (Join-Path $priWork 'Assets') -Recurse
            Copy-Item (Join-Path $work 'AppxManifest.xml') $priWork
            & $makepri createconfig /cf (Join-Path $priWork 'priconfig.xml') /dq en-US /o | Out-Null
            if ($LASTEXITCODE) { throw 'makepri createconfig failed' }
            & $makepri new /pr $priWork /cf (Join-Path $priWork 'priconfig.xml') /mn (Join-Path $priWork 'AppxManifest.xml') /of (Join-Path $work 'resources.pri') /o | Out-Null
            if ($LASTEXITCODE) { throw 'makepri new failed' }
            Remove-Item $priWork -Recurse -Force
        } catch { Write-Warning "resources.pri not made ($($_.Exception.Message)); packing without it." }

        if (Test-Path $out) { Remove-Item $out -Force }
        & $sdkMakeAppx pack /d $work /p $out /o
        if ($LASTEXITCODE) { throw "makeappx failed for $name" }
        Write-Host "$name : $out ($([math]::Round((Get-Item $out).Length / 1MB, 1)) MB)"
    } finally {
        Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
    }
}

# 1. The Store package: unsigned. Partner Center accepts it and Microsoft signs it.
$storePackage = Join-Path $OutputDir "ClipItTo-$version.msix"
New-Msix 'Store package (unsigned)' $IdentityName $Publisher $PublisherDisplayName $storePackage

# 2. Optional: a copy for sideloading on this PC. The publisher in the manifest must equal the certificate's subject, so this is
#    a second package with another identity (it never replaces or collides with the Store package).
if ($TestSign) {
    $signtool = Find-SdkTool 'signtool.exe'
    $cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $testSubject -and $_.NotAfter -gt (Get-Date).AddDays(7) } |
        Select-Object -First 1
    if (-not $cert) {
        $cert = New-SelfSignedCertificate -Type Custom -Subject $testSubject -KeyUsage DigitalSignature -FriendlyName 'ClipItTo test certificate' `
            -CertStoreLocation 'Cert:\CurrentUser\My' -NotAfter (Get-Date).AddMonths(12) `
            -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}')
        Write-Host "Created the test certificate $($cert.Thumbprint) (CurrentUser\My)."
    }
    $cer = Join-Path $OutputDir 'ClipItTo-Test.cer'
    Export-Certificate -Cert $cert -FilePath $cer | Out-Null
    $testPackage = Join-Path $OutputDir "ClipItTo-$version-test.msix"
    New-Msix 'Test package' $testPackageName $testSubject 'ClipItTo Test' $testPackage
    & $signtool sign /fd SHA256 /sha1 $cert.Thumbprint $testPackage
    if ($LASTEXITCODE) { throw 'signtool failed' }
    Write-Host @"

Test package: $testPackage   (signed with the self-signed certificate $($cert.Thumbprint))
To try it on this PC:
  1. Trust the certificate (elevated PowerShell, once):
       Import-Certificate -FilePath '$cer' -CertStoreLocation Cert:\LocalMachine\TrustedPeople
  2. Install:   Add-AppxPackage '$testPackage'
  3. Remove the package and the certificate afterwards:   powershell -File scripts\make-msix.ps1 -CleanupTest   (elevated)
Quit an installed ClipItTo (the installer version) first: two daemons would fight over the hotkey.
"@
}
