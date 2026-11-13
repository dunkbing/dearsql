; the windows installer (inno setup 6), built by scripts/build-installer.ps1:
;   iscc /DAppVersion=<x.y.z> /DStageDir=<exe, dll, tools\> /DSourceDir=<repo> /O<out> DearSQL.iss
; per user, no admin. run by an update with /verysilent /update=true it stages the new version
; in {app}\install, and imshell-update.exe swaps it in once the app exits (imshell docs/auto-update.md).
; it replaces the msi earlier versions shipped: that one is removed first, keeping its folder

[Setup]
AppId={{CADA3B17-5B67-4858-8C4D-A77B24D25FAB}
AppName=DearSQL
AppVersion={#AppVersion}
AppVerName=DearSQL {#AppVersion}
AppPublisher=DearSQL
AppPublisherURL=https://dearsql.dev
VersionInfoVersion={#AppVersion}
DefaultDirName={code:DefaultDir}
UsePreviousAppDir=yes
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputBaseFilename=DearSQL-x64-setup
SetupIconFile={#SourceDir}\assets\appicon.ico
UninstallDisplayIcon={app}\dearsql.exe
LicenseFile={#SourceDir}\packaging\windows\License.rtf
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
ChangesAssociations=yes
; a running copy holds dearsql.exe: closed for a normal install. an update writes only
; to {app}\install and tools\, which nothing holds
CloseApplications=force
RestartApplications=no

[Tasks]
Name: desktopicon; Description: "Create a desktop shortcut"

[Files]
; everything the running app loads goes to InstallDir; the helper runs from a %TEMP% copy
Source: "{#StageDir}\dearsql.exe"; DestDir: "{code:InstallDir}"; Flags: ignoreversion
Source: "{#StageDir}\WinSparkle.dll"; DestDir: "{code:InstallDir}"; Flags: ignoreversion
Source: "{#StageDir}\tools\imshell-update.exe"; DestDir: "{app}\tools"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\DearSQL"; Filename: "{app}\dearsql.exe"; Comment: "Cross-platform SQL database client"
Name: "{autodesktop}\DearSQL"; Filename: "{app}\dearsql.exe"; Comment: "Cross-platform SQL database client"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Classes\.db"; ValueType: string; ValueData: "DearSQL.Database"; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\.sqlite"; ValueType: string; ValueData: "DearSQL.Database"; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\.sqlite3"; ValueType: string; ValueData: "DearSQL.Database"; Flags: uninsdeletevalue
Root: HKCU; Subkey: "Software\Classes\DearSQL.Database"; ValueType: string; ValueData: "SQLite Database"; Flags: uninsdeletekey
Root: HKCU; Subkey: "Software\Classes\DearSQL.Database\DefaultIcon"; ValueType: string; ValueData: "{app}\dearsql.exe,0"
Root: HKCU; Subkey: "Software\Classes\DearSQL.Database\shell\open\command"; ValueType: string; ValueData: """{app}\dearsql.exe"" ""%1"""

[Run]
; silent too (no skipifsilent): an update from an msi version reopens the app. a staged update
; is reopened by the helper; `dearsql upgrade` passes /launch=no
Filename: "{app}\dearsql.exe"; Description: "Launch DearSQL"; Flags: nowait postinstall; Check: ShouldLaunch

[UninstallDelete]
; a staged update logged its files under install\, then the helper moved them up
Type: files; Name: "{app}\dearsql.exe"
Type: files; Name: "{app}\WinSparkle.dll"
Type: files; Name: "{app}\update.log"
Type: files; Name: "{app}\.imshell-reopen"
Type: filesandordirs; Name: "{app}\install"
Type: filesandordirs; Name: "{app}\old"
Type: filesandordirs; Name: "{app}\tools"
Type: dirifempty; Name: "{app}"

[Code]
const
  MsiUpgradeCode = '{E7A1C3D5-4B2F-4E8A-9F1D-6C3B5A7D8E9F}';

var
  MsiProduct: string; // the msi install being replaced, if any
  MsiDir: string;

function MsiEnumRelatedProducts(UpgradeCode: string; Reserved, Index: DWORD; ProductCode: string): Integer;
  external 'MsiEnumRelatedProductsW@msi.dll stdcall';

function IsUpdating(): Boolean;
begin
  Result := (ExpandConstant('{param:update|false}') = 'true') and WizardSilent();
end;

function ShouldLaunch(): Boolean;
begin
  Result := not IsUpdating() and (ExpandConstant('{param:launch|yes}') <> 'no');
end;

function InstallDir(Param: string): string;
begin
  if IsUpdating() then
    Result := ExpandConstant('{app}\install')
  else
    Result := ExpandConstant('{app}');
end;

function DefaultDir(Param: string): string;
begin
  if MsiDir <> '' then
    Result := MsiDir
  else
    Result := ExpandConstant('{localappdata}\DearSQL');
end;

function InitializeSetup(): Boolean;
var
  Code, Cmd: string;
  I: Integer;
begin
  SetLength(Code, 39);
  if MsiEnumRelatedProducts(MsiUpgradeCode, 0, 0, Code) = 0 then
  begin
    MsiProduct := Copy(Code, 1, 38);
    // the msi kept no record of its folder; its file association names the exe
    if RegQueryStringValue(HKCU, 'Software\Classes\DearSQL.Database\shell\open\command', '', Cmd) and
       (Copy(Cmd, 1, 1) = '"') then
    begin
      Cmd := Copy(Cmd, 2, Length(Cmd));
      I := Pos('"', Cmd);
      if I > 0 then
        MsiDir := ExtractFileDir(Copy(Cmd, 1, I - 1));
    end;
    Log('replacing msi ' + MsiProduct + ' in ' + DefaultDir(''));
  end;
  Result := True;
end;

// after the preparing step closed a running copy, before any file is written
procedure CurStepChanged(CurStep: TSetupStep);
var
  Code: Integer;
begin
  if (CurStep <> ssInstall) or (MsiProduct = '') then
    Exit;
  if Exec(ExpandConstant('{sys}\msiexec.exe'), '/x ' + MsiProduct + ' /qn REBOOT=ReallySuppress', '',
          SW_HIDE, ewWaitUntilTerminated, Code) and ((Code = 0) or (Code = 3010)) then
    Log('msi removed, exit code ' + IntToStr(Code))
  else
    // installing over it still works; the old entry stays in installed apps
    Log('msi removal failed, exit code ' + IntToStr(Code));
end;
