; Inno Setup script for ClipItTo. Packs the output of `cmake --install` into a per-user
; installer (no admin rights needed).
;   iscc /DStageDir=C:\path\to\stage /DAppVersion=0.1.0 [/DOutputDir=...] clipitto.iss

#ifndef StageDir
  #error Define StageDir: the directory produced by "cmake --install build/windows --prefix <absolute dir>"
#endif
#ifndef AppVersion
  #define AppVersion "0.10.1"
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
SetupIconFile=ClipItTo.ico
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

[Code]
// Is clipitto-daemon.exe running? `find` ends with 0 when tasklist's answer contains the name.
function DaemonIsRunning: Boolean;
var
  ResultCode: Integer;
begin
  Result := Exec(ExpandConstant('{cmd}'),
    '/C tasklist /FI "IMAGENAME eq clipitto-daemon.exe" /NH | find /I "clipitto-daemon.exe"',
    '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

// Stop a running ClipItTo so its files can be replaced or removed: ask it to quit, give it a few seconds, then end it.
// (A daemon busy with a stuck translation, or one started from another folder, may not quit on its own.)
procedure StopClipItTo;
var
  ResultCode, I: Integer;
begin
  if FileExists(ExpandConstant('{app}\bin\clipitto.exe')) then
    Exec(ExpandConstant('{app}\bin\clipitto.exe'), '--quit', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  for I := 1 to 10 do
  begin
    if not DaemonIsRunning then
      Break;
    Sleep(500);
  end;
  if DaemonIsRunning then
  begin
    Exec(ExpandConstant('{sys}\taskkill.exe'), '/F /T /IM clipitto-daemon.exe', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
    Sleep(1000);
  end;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  StopClipItTo;
  Result := '';
end;

function InitializeUninstall: Boolean;
begin
  StopClipItTo;
  Result := True;
end;
