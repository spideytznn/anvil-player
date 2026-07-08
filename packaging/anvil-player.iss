#define AppName "Anvil Player"
#define AppPublisher "Anvil"
#define AppVersion "0.1.0"
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

[Run]
Filename: "{app}\AnvilPlayer.App.exe"; Description: "{cm:LaunchProgram,Anvil Player}"; Flags: nowait postinstall skipifsilent
