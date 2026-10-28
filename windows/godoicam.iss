; Instalador único do GodoiCam (Inno Setup 6).
; Gera GodoiCam-Setup.exe: copia o app, registra a câmera virtual, libera o firewall.
; Não inclui o adb (licença da Google). USB continua pedindo platform-tools ou o SDK.
#define AppVersion "0.1.0"

[Setup]
AppId={{A7E31C4B-9D2F-4E18-8B70-6C1A0F3D5E92}
AppName=GodoiCam
AppVersion={#AppVersion}
AppPublisher=Ramon Godoi
AppPublisherURL=https://github.com/ramongodoii/GodoiCam
AppSupportURL=https://github.com/ramongodoii/GodoiCam/issues
DefaultDirName={autopf}\GodoiCam
DefaultGroupName=GodoiCam
DisableProgramGroupPage=yes
LicenseFile=..\LICENSE
OutputDir=bin
OutputBaseFilename=GodoiCam-Setup
SetupIconFile=app\godoicam.ico
UninstallDisplayIcon={app}\GodoiCam.exe
Compression=lzma2
SolidCompression=yes
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.22000
WizardStyle=modern
CloseApplications=yes
RestartIfNeededByRun=no
VersionInfoVersion={#AppVersion}
VersionInfoDescription=GodoiCam - celular como webcam

[Languages]
Name: "ptbr"; MessagesFile: "compiler:Languages\BrazilianPortuguese.isl"

[Tasks]
Name: "desktopicon"; Description: "Criar um atalho na área de trabalho"; GroupDescription: "Atalhos:"; Flags: unchecked

[Files]
Source: "bin\GodoiCam.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "bin\GodoiCamVCam.dll"; DestDir: "{app}"; Flags: ignoreversion restartreplace uninsrestartdelete

[Icons]
Name: "{group}\GodoiCam"; Filename: "{app}\GodoiCam.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\GodoiCam"; Filename: "{app}\GodoiCam.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{sys}\regsvr32.exe"; Parameters: "/s ""{app}\GodoiCamVCam.dll"""; Flags: runhidden; StatusMsg: "Registrando a câmera virtual…"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=GodoiCam"; Flags: runhidden skipifdoesntexist
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=GodoiCam dir=in action=allow program=""{app}\GodoiCam.exe"" enable=yes profile=any"; Flags: runhidden
Filename: "{app}\GodoiCam.exe"; Description: "Abrir o GodoiCam agora"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{cmd}"; Parameters: "/c taskkill /im GodoiCam.exe /f"; Flags: runhidden; RunOnceId: "KillApp"
Filename: "{app}\GodoiCam.exe"; Parameters: "--remove-camera"; Flags: runhidden; RunOnceId: "RemoveVCam"
Filename: "{sys}\regsvr32.exe"; Parameters: "/s /u ""{app}\GodoiCamVCam.dll"""; Flags: runhidden; RunOnceId: "UnregDll"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=GodoiCam"; Flags: runhidden; RunOnceId: "FwDel"

[Code]
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  ResultCode: Integer;
begin
  Exec(ExpandConstant('{cmd}'), '/c taskkill /im GodoiCam.exe /f', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Exec(ExpandConstant('{cmd}'), '/c taskkill /im GodoiCan.exe /f', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  if FileExists(ExpandConstant('{autopf}\GodoiCan\GodoiCan.exe')) then
    Exec(ExpandConstant('{autopf}\GodoiCan\GodoiCan.exe'), '--remove-camera', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  if FileExists(ExpandConstant('{autopf}\GodoiCan\GodoiCanVCam.dll')) then
    Exec(ExpandConstant('{sys}\regsvr32.exe'), '/s /u "' + ExpandConstant('{autopf}\GodoiCan\GodoiCanVCam.dll') + '"', '', SW_HIDE, ewWaitUntilTerminated, ResultCode);
  Result := '';
end;
