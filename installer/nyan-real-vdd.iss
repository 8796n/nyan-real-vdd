; Inno Setup script for the nyan Real VDD driver package.
;
; scripts\package.ps1 stages the portable payload (driver package, certificate,
; nyanvddctl and the install/uninstall scripts) and passes its path via
; /DStageDir. The installed layout is exactly the portable layout, so the same
; install.ps1 / uninstall.ps1 that the ZIP ships drive the installer too —
; there is one implementation of "trust the certificate, pnputil, create the
; device node", not two.

#ifndef AppVersion
  #define AppVersion "0.0.0-noversion"
#endif
#ifndef StageDir
  ; Fallback for compiling by hand; package.ps1 overrides this.
  #define StageDir "..\out\stage-installer"
#endif

[Setup]
AppId={{9C74A1AB-C7A0-402A-8811-1593BF3D1E11}
AppName=nyan Real VDD
AppVersion={#AppVersion}
AppPublisher=8796n
AppPublisherURL=https://github.com/8796n/nyan-real-vdd
DefaultDirName={autopf}\nyan Real VDD
DefaultGroupName=nyan Real VDD
DisableProgramGroupPage=yes
; A driver is machine-wide: this cannot be a per-user install.
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; The INF only binds on Windows 11 24H2 and later. Refusing here gives a clear
; message instead of a device node that never gets a driver.
MinVersion=10.0.26100
LicenseFile=..\LICENSE
OutputBaseFilename=nyan-real-vdd-{#AppVersion}-windows-x64-installer
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
UninstallDisplayName=nyan Real VDD

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#StageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\nyan Real VDD README"; Filename: "{app}\README.txt"
Name: "{group}\{cm:UninstallProgram,nyan Real VDD}"; Filename: "{uninstallexe}"

[Code]
// Inno Setup's [Code] runs in a 32-bit process. pnputil.exe exists only under
// the real System32 (unlike certutil.exe, which is in both), so a PowerShell
// started here lands in SysWOW64 and dies on "pnputil is not recognized" —
// an error that points nowhere near the cause.
//
// Naming {sys} is NOT enough: the path handed to CreateProcess is still
// subject to WOW64 redirection, and the install transcript confirms the child
// still came up 32-bit. install.ps1 re-launches itself through Sysnative for
// exactly this reason, and that is what actually fixes it. This stays because
// it costs nothing and states the intent.
function NativePowerShell(): string;
begin
  Result := ExpandConstant('{sys}\WindowsPowerShell\v1.0\powershell.exe');
end;

// Logs go next to the driver's own log and survive uninstall, so a failed
// unattended install still leaves something to read.
function LogPathFor(Stem: string): string;
begin
  Result := ExpandConstant('{commonappdata}\nyan-real-vdd\' + Stem + '.log');
end;

function ScriptCommand(ScriptName: string; Stem: string; Args: string): string;
begin
  Result := '-NoProfile -ExecutionPolicy Bypass -File "' +
            ExpandConstant('{app}\' + ScriptName) + '"' +
            ' -LogPath "' + LogPathFor(Stem) + '"';
  if Args <> '' then
    Result := Result + ' ' + Args;
end;

procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
begin
  if CurStep <> ssPostInstall then
    Exit;

  // install.ps1 trusts the signing certificate, stages the driver with
  // pnputil and creates the device node.
  if (not Exec(NativePowerShell, ScriptCommand('install.ps1', 'install', ''), '',
               SW_HIDE, ewWaitUntilTerminated, ResultCode)) or (ResultCode <> 0) then
  begin
    // The files are installed, so this is recoverable by hand: say exactly how
    // rather than rolling back and hiding the reason. SuppressibleMsgBox, not
    // MsgBox, so /SUPPRESSMSGBOXES really suppresses it and an unattended
    // install cannot block on a dialog nobody is there to click.
    SuppressibleMsgBox('The files were installed, but registering the driver failed'
           + #13#10 + '(exit code ' + IntToStr(ResultCode) + ').'
           + #13#10 + #13#10
           + 'What happened is recorded in:'
           + #13#10 + LogPathFor('install')
           + #13#10 + #13#10
           + 'To retry, run this from an elevated PowerShell:'
           + #13#10 + 'powershell -NoProfile -ExecutionPolicy Bypass -File "'
           + ExpandConstant('{app}\install.ps1') + '"',
           mbError, MB_OK, IDOK);
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  ResultCode: Integer;
begin
  // usUninstall runs before the files are removed, which is what lets the
  // uninstaller call the script that is about to be deleted.
  if CurUninstallStep <> usUninstall then
    Exit;

  if (not Exec(NativePowerShell, ScriptCommand('uninstall.ps1', 'uninstall', '-RemoveCert'), '',
               SW_HIDE, ewWaitUntilTerminated, ResultCode)) or (ResultCode <> 0) then
  begin
    SuppressibleMsgBox('Removing the driver failed (exit code ' + IntToStr(ResultCode) + ').'
           + #13#10 + #13#10
           + 'Details: ' + LogPathFor('uninstall')
           + #13#10 + #13#10
           + 'The files will still be removed. To finish cleaning up, open an'
           + #13#10 + 'elevated PowerShell and run:'
           + #13#10 + 'Get-WindowsDriver -Online -All | Where-Object {'
           + #13#10 + '  $_.OriginalFileName -match ''nyanvdd\.inf$'' } |'
           + #13#10 + '  ForEach-Object { pnputil /delete-driver'
           + #13#10 + '    (Split-Path $_.Driver -Leaf) /uninstall }',
           mbError, MB_OK, IDOK);
  end;
end;
