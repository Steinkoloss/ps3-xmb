; PS3 XMB wallpaper for Windows (Inno Setup). Installs per user, no administrator rights.
; Built by .github/workflows/release.yml from the deployed app in dist\.
#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif

[Setup]
AppName=PS3 XMB
AppVersion={#AppVersion}
AppPublisher=Steinkoloss
AppPublisherURL=https://github.com/Steinkoloss/ps3-xmb
DefaultDirName={localappdata}\Programs\PS3 XMB
DefaultGroupName=PS3 XMB
DisableProgramGroupPage=yes
DisableDirPage=yes
PrivilegesRequired=lowest
OutputBaseFilename=PS3-XMB-Setup
LicenseFile=..\..\LICENSE
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=force
UninstallDisplayIcon={app}\ps3-xmb.exe

[Files]
Source: "..\..\dist\*"; DestDir: "{app}"; Flags: recursesubdirs ignoreversion

[Icons]
Name: "{group}\PS3 XMB"; Filename: "{app}\ps3-xmb.exe"
Name: "{group}\Uninstall PS3 XMB"; Filename: "{uninstallexe}"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: none; ValueName: "PS3 XMB"; Flags: uninsdeletevalue

[Run]
Filename: "{app}\ps3-xmb.exe"; Description: "Start the PS3 XMB wallpaper"; Flags: postinstall nowait

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/im ps3-xmb.exe /f"; Flags: runhidden; RunOnceId: "StopWallpaper"

[UninstallDelete]
Type: filesandordirs; Name: "{localappdata}\ps3-xmb"
