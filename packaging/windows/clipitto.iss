; Inno Setup script for ClipItTo. Packs the output of `cmake --install` into a per-user
; installer (no admin rights needed).
;   iscc /DStageDir=C:\path\to\stage /DAppVersion=0.1.0 [/DOutputDir=...] clipitto.iss

#ifndef StageDir
  #error Define StageDir: the directory produced by "cmake --install build/windows --prefix <absolute dir>"
#endif
#ifndef AppVersion
  #define AppVersion "0.2.0"
#endif
#ifndef OutputDir
  #define OutputDir "Output"
#endif

[Setup]
AppId={{8C4D2B7E-5A61-4F0B-9E3C-2D7F1A6B9C40}
AppName=ClipItTo
AppVersion={#AppVersion}
AppPublisher=NiMiZ
AppPublisherURL=https://github.com/NiMiZ/ClipItTo
DefaultDirName={autopf}\ClipItTo
DefaultGroupName=ClipItTo
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
OutputDir={#OutputDir}
OutputBaseFilename=ClipItTo-{#AppVersion}-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\bin\clipitto-daemon.exe
UninstallDisplayName=ClipItTo

[Tasks]
Name: autostart; Description: "Start ClipItTo when I sign in"; GroupDescription: "Startup:"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{group}\ClipItTo"; Filename: "{app}\bin\clipitto-daemon.exe"
Name: "{autostartup}\ClipItTo"; Filename: "{app}\bin\clipitto-daemon.exe"; Tasks: autostart

[Run]
Filename: "{app}\bin\clipitto-daemon.exe"; Description: "Start ClipItTo now"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{app}\bin\clipitto.exe"; Parameters: "--quit"; Flags: runhidden; RunOnceId: "StopClipItTo"

[Code]
// Stop a running ClipItTo so an upgrade can replace its files.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
begin
  if FileExists(ExpandConstant('{app}\bin\clipitto.exe')) then
  begin
    Exec(ExpandConstant('{app}\bin\clipitto.exe'), '--quit', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Sleep(1000);
  end;
  Result := '';
end;
