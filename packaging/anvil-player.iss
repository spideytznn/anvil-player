#define AppName "Anvil Player"
#define AppPublisher "Anvil"
#ifndef AppVersion
#define AppVersion "1.0"
#endif
#define SourceRoot "..\dist\release\staging\AnvilPlayer"

[Setup]
AppId={{4B6E748C-C7F2-4E4D-8B42-403632385B54}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={autopf}\Anvil Player
DefaultGroupName=Anvil Player
DisableProgramGroupPage=yes
LicenseFile={#SourceRoot}\LICENSE
OutputDir=..\dist\release
OutputBaseFilename=AnvilPlayer-{#AppVersion}-x64-Setup
SetupIconFile={#SourceRoot}\assets\app.ico
Compression=lzma2/fast
SolidCompression=no
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
UninstallDisplayIcon={app}\AnvilPlayer.App.exe

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SourceRoot}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\Anvil Player"; Filename: "{app}\AnvilPlayer.App.exe"; WorkingDir: "{app}"; IconFilename: "{app}\assets\app.ico"
Name: "{autodesktop}\Anvil Player"; Filename: "{app}\AnvilPlayer.App.exe"; WorkingDir: "{app}"; IconFilename: "{app}\assets\app.ico"; Tasks: desktopicon

[Registry]
Root: HKLM; Subkey: "Software\RegisteredApplications"; ValueType: string; ValueName: "AnvilPlayer"; ValueData: "Software\AnvilPlayer\Capabilities"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\AnvilPlayer\Capabilities"; ValueType: string; ValueName: "ApplicationName"; ValueData: "Anvil Player"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\AnvilPlayer\Capabilities"; ValueType: string; ValueName: "ApplicationDescription"; ValueData: "Anvil Player media playback application"
Root: HKLM; Subkey: "Software\AnvilPlayer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mp4"; ValueData: "AnvilPlayer.Media"
Root: HKLM; Subkey: "Software\AnvilPlayer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mkv"; ValueData: "AnvilPlayer.Media"
Root: HKLM; Subkey: "Software\AnvilPlayer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".mov"; ValueData: "AnvilPlayer.Media"
Root: HKLM; Subkey: "Software\AnvilPlayer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".m2ts"; ValueData: "AnvilPlayer.Media"
Root: HKLM; Subkey: "Software\AnvilPlayer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".ts"; ValueData: "AnvilPlayer.Media"
Root: HKLM; Subkey: "Software\AnvilPlayer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".webm"; ValueData: "AnvilPlayer.Media"
Root: HKLM; Subkey: "Software\AnvilPlayer\Capabilities\FileAssociations"; ValueType: string; ValueName: ".avi"; ValueData: "AnvilPlayer.Media"
Root: HKLM; Subkey: "Software\Classes\AnvilPlayer.Media"; ValueType: string; ValueData: "Media file"; Flags: uninsdeletekey
Root: HKLM; Subkey: "Software\Classes\AnvilPlayer.Media\DefaultIcon"; ValueType: string; ValueData: "{app}\AnvilPlayer.App.exe,0"
Root: HKLM; Subkey: "Software\Classes\AnvilPlayer.Media\shell\open\command"; ValueType: string; ValueData: """{app}\AnvilPlayer.App.exe"" --play ""%1"""
Root: HKLM; Subkey: "Software\Classes\.mp4\OpenWithProgids"; ValueType: none; ValueName: "AnvilPlayer.Media"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.mkv\OpenWithProgids"; ValueType: none; ValueName: "AnvilPlayer.Media"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.mov\OpenWithProgids"; ValueType: none; ValueName: "AnvilPlayer.Media"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.m2ts\OpenWithProgids"; ValueType: none; ValueName: "AnvilPlayer.Media"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.ts\OpenWithProgids"; ValueType: none; ValueName: "AnvilPlayer.Media"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.webm\OpenWithProgids"; ValueType: none; ValueName: "AnvilPlayer.Media"; Flags: uninsdeletevalue
Root: HKLM; Subkey: "Software\Classes\.avi\OpenWithProgids"; ValueType: none; ValueName: "AnvilPlayer.Media"; Flags: uninsdeletevalue

[Run]
Filename: "{app}\AnvilPlayer.App.exe"; Description: "{cm:LaunchProgram,Anvil Player}"; Flags: nowait postinstall skipifsilent
