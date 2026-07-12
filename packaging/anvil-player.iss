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
CloseApplications=yes
RestartApplications=no

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

; The application also registers per-user file associations when it starts.
; Do not create these keys during setup; only remove Anvil Player's entries
; during uninstall, leaving other applications' extension keys intact.
Root: HKCU; Subkey: "Software\RegisteredApplications"; ValueName: "AnvilPlayer"; Flags: uninsdeletevalue dontcreatekey
Root: HKCU; Subkey: "Software\Classes\AnvilPlayer"; Flags: uninsdeletekey dontcreatekey
Root: HKCU; Subkey: "Software\Classes\AnvilPlayer.Media"; Flags: uninsdeletekey dontcreatekey
Root: HKCU; Subkey: "Software\Classes\.mp4\OpenWithProgids"; ValueName: "AnvilPlayer.Media"; Flags: uninsdeletevalue dontcreatekey
Root: HKCU; Subkey: "Software\Classes\.mkv\OpenWithProgids"; ValueName: "AnvilPlayer.Media"; Flags: uninsdeletevalue dontcreatekey
Root: HKCU; Subkey: "Software\Classes\.mov\OpenWithProgids"; ValueName: "AnvilPlayer.Media"; Flags: uninsdeletevalue dontcreatekey
Root: HKCU; Subkey: "Software\Classes\.m2ts\OpenWithProgids"; ValueName: "AnvilPlayer.Media"; Flags: uninsdeletevalue dontcreatekey
Root: HKCU; Subkey: "Software\Classes\.ts\OpenWithProgids"; ValueName: "AnvilPlayer.Media"; Flags: uninsdeletevalue dontcreatekey
Root: HKCU; Subkey: "Software\Classes\.webm\OpenWithProgids"; ValueName: "AnvilPlayer.Media"; Flags: uninsdeletevalue dontcreatekey
Root: HKCU; Subkey: "Software\Classes\.avi\OpenWithProgids"; ValueName: "AnvilPlayer.Media"; Flags: uninsdeletevalue dontcreatekey

[Run]
Filename: "{app}\AnvilPlayer.App.exe"; Description: "{cm:LaunchProgram,Anvil Player}"; Flags: nowait postinstall skipifsilent

[Code]
var
  DeleteUserData: Boolean;

function ShowUninstallOptions(): Boolean;
var
  OptionsForm: TSetupForm;
  TitleLabel, DetailLabel: TNewStaticText;
  DeleteDataCheckBox: TNewCheckBox;
  ContinueButton, CancelButton: TNewButton;
  ButtonWidth: Integer;
begin
  Result := False;
  OptionsForm := CreateCustomForm(ScaleX(420), ScaleY(180), True, True);
  try
    OptionsForm.Caption := '卸载 Anvil Player';

    TitleLabel := TNewStaticText.Create(OptionsForm);
    TitleLabel.Parent := OptionsForm;
    TitleLabel.Left := ScaleX(20);
    TitleLabel.Top := ScaleY(18);
    TitleLabel.Width := OptionsForm.ClientWidth - ScaleX(40);
    TitleLabel.AutoSize := False;
    TitleLabel.Height := ScaleY(24);
    TitleLabel.Caption := '选择是否同时删除用户数据';
    TitleLabel.Font.Style := [fsBold];

    DetailLabel := TNewStaticText.Create(OptionsForm);
    DetailLabel.Parent := OptionsForm;
    DetailLabel.Left := ScaleX(20);
    DetailLabel.Top := ScaleY(48);
    DetailLabel.Width := OptionsForm.ClientWidth - ScaleX(40);
    DetailLabel.Height := ScaleY(44);
    DetailLabel.AutoSize := False;
    DetailLabel.WordWrap := True;
    DetailLabel.Caption := '不勾选时将保留媒体库、播放列表、账号登录信息、播放记录和个性化设置，方便以后重新安装。';

    DeleteDataCheckBox := TNewCheckBox.Create(OptionsForm);
    DeleteDataCheckBox.Parent := OptionsForm;
    DeleteDataCheckBox.Left := ScaleX(20);
    DeleteDataCheckBox.Top := ScaleY(104);
    DeleteDataCheckBox.Width := OptionsForm.ClientWidth - ScaleX(40);
    DeleteDataCheckBox.Height := ScaleY(24);
    DeleteDataCheckBox.Caption := '删除所有用户信息和本地记录';
    DeleteDataCheckBox.Checked := False;

    ContinueButton := TNewButton.Create(OptionsForm);
    ContinueButton.Parent := OptionsForm;
    ContinueButton.Caption := '继续';
    ContinueButton.Top := OptionsForm.ClientHeight - ScaleY(33);
    ContinueButton.Height := ScaleY(23);
    ContinueButton.ModalResult := mrOk;
    ContinueButton.Default := True;

    CancelButton := TNewButton.Create(OptionsForm);
    CancelButton.Parent := OptionsForm;
    CancelButton.Caption := '取消';
    CancelButton.Top := ContinueButton.Top;
    CancelButton.Height := ContinueButton.Height;
    CancelButton.ModalResult := mrCancel;
    CancelButton.Cancel := True;

    ButtonWidth := OptionsForm.CalculateButtonWidth([ContinueButton.Caption, CancelButton.Caption]);
    ContinueButton.Width := ButtonWidth;
    CancelButton.Width := ButtonWidth;
    CancelButton.Left := OptionsForm.ClientWidth - ScaleX(10) - ButtonWidth;
    ContinueButton.Left := CancelButton.Left - ScaleX(6) - ButtonWidth;

    OptionsForm.ActiveControl := DeleteDataCheckBox;
    if OptionsForm.ShowModal() = mrOk then
    begin
      DeleteUserData := DeleteDataCheckBox.Checked;
      Result := True;
    end;
  finally
    OptionsForm.Free();
  end;
end;

function InitializeUninstall(): Boolean;
begin
  DeleteUserData := False;
  if UninstallSilent() then
    Result := True
  else
    Result := ShowUninstallOptions();
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if (CurUninstallStep = usPostUninstall) and DeleteUserData then
  begin
    { WebView2 profiles contain the media library, playlists, credentials,
      UI preferences and caches. The root also contains recent-media data. }
    DelTree(ExpandConstant('{localappdata}\AnvilPlayer'), True, True, True);
    { Fallback folders used when LocalAppData is unavailable. }
    DelTree(ExpandConstant('{tmp}\AnvilPlayer-WebView2'), True, True, True);
    DelTree(ExpandConstant('{tmp}\anvil-player'), True, True, True);
    RegDeleteKeyIncludingSubkeys(HKCU, 'Software\AnvilPlayer');
  end;
end;
