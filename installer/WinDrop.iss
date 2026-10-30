; WinDrop setup (Inno Setup 6). Built by tools/release.ps1, which passes /DAppVersion and /DExe:
;   ISCC.exe /DAppVersion=0.1.0 /DExe=..\windows\build-msvc\WinDrop.exe installer\WinDrop.iss

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef Exe
  #define Exe "..\windows\build-msvc\WinDrop.exe"
#endif

[Setup]
AppId={{6B1F3C2E-8A4D-4E7B-9C51-2D7A0E5B9F13}
AppName=WinDrop
AppVersion={#AppVersion}
AppVerName=WinDrop {#AppVersion}
AppPublisher=WinDrop
AppPublisherURL=https://github.com/Skyblock127/WinDrop
AppSupportURL=https://github.com/Skyblock127/WinDrop/issues
AppUpdatesURL=https://github.com/Skyblock127/WinDrop/releases
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\WinDrop
DefaultGroupName=WinDrop
DisableProgramGroupPage=yes
; Admin so setup can add the firewall rule; then Windows never asks on first run.
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.19041
OutputDir=..\dist
OutputBaseFilename=WinDrop-Setup
SetupIconFile=..\windows\res\app.ico
UninstallDisplayIcon={app}\WinDrop.exe
UninstallDisplayName=WinDrop
WizardStyle=modern
WizardImageFile=wizard.bmp
WizardSmallImageFile=wizard-small.bmp
Compression=lzma2/max
SolidCompression=yes
CloseApplications=force
RestartApplications=no
UsedUserAreasWarning=no

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Tasks]
; Not offered on an update: from then on the app's own "Start with Windows" switch is in charge.
Name: "startup"; Description: "Start WinDrop when I sign in to Windows (recommended)"; Check: not IsUpgrade
Name: "desktopicon"; Description: "Create a desktop shortcut"; Flags: unchecked

[Files]
Source: "{#Exe}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\WinDrop"; Filename: "{app}\WinDrop.exe"; Comment: "Send files between this PC and your phone"
Name: "{autodesktop}\WinDrop"; Filename: "{app}\WinDrop.exe"; Tasks: desktopicon

[Registry]
; Same entry the app's "Start with Windows" toggles, listed in Settings > Apps > Startup.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "WinDrop"; \
  ValueData: """{app}\WinDrop.exe"""; Tasks: startup; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run"; ValueType: binary; \
  ValueName: "WinDrop"; ValueData: "02 00 00 00 00 00 00 00 00 00 00 00"; Tasks: startup; Flags: uninsdeletevalue

[Run]
; Inbound TCP only for WinDrop.exe. The Wi-Fi Direct link counts as a Public network, so both profiles.
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""WinDrop"""; Flags: runhidden
Filename: "{sys}\netsh.exe"; \
  Parameters: "advfirewall firewall add rule name=""WinDrop"" dir=in action=allow protocol=TCP program=""{app}\WinDrop.exe"" profile=private,public enable=yes"; \
  Flags: runhidden; StatusMsg: "Allowing WinDrop through the firewall..."
Filename: "{app}\WinDrop.exe"; Description: "Start WinDrop now"; Flags: nowait postinstall skipifsilent runasoriginaluser
; In-app updates run setup silently: start the new version straight away.
Filename: "{app}\WinDrop.exe"; Flags: nowait runasoriginaluser; Check: WizardSilent

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/f /im WinDrop.exe"; Flags: runhidden; RunOnceId: "StopApp"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""WinDrop"""; Flags: runhidden; RunOnceId: "Firewall"

[UninstallDelete]
Type: files; Name: "{usersendto}\Phone (WinDrop).lnk"

[Code]
var
  Upgrade: Boolean;

function InitializeSetup(): Boolean;
begin
  Upgrade := RegKeyExists(HKLM, 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{6B1F3C2E-8A4D-4E7B-9C51-2D7A0E5B9F13}_is1');
  Result := True;
end;

function IsUpgrade(): Boolean;
begin
  Result := Upgrade;
end;

// The app may have made the startup entry itself (tray > Start with Windows).
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
  begin
    RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'WinDrop');
    RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run', 'WinDrop');
  end;
end;

// The tray app has no visible window for Restart Manager to close, so stop it before copying files.
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Code: Integer;
begin
  Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /im WinDrop.exe', '', SW_HIDE, ewWaitUntilTerminated, Code);
  Result := '';
end;
