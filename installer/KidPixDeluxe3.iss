; Kid Pix Deluxe 3 - installer for Windows 10 and 11
;
; Installs the patched game (cnc-ddraw scaling, the configit.dll compatibility
; shim, the QuickTime stand-in) together with Kid Pix Launcher, for the
; current user only. No administrator rights are needed or asked for.
;
; Why per-user: the game keeps every child's pictures in Users\ inside its own
; folder, and the launcher rewrites ddraw.ini before each start. Both need a
; folder the user can write to, which Program Files is not.
;
; Build with ..\build.ps1. This script reads the game that ..\prepare.ps1
; made from the CD (build\game, build\fonts); pass /DGame=... /DFonts=... to
; ISCC to use other folders.

#define AppName      "Kid Pix Deluxe 3"
#define AppVersion   "1.0.3"
#define Root         ".."
#ifndef Game
  #define Game       Root + "\build\game"
#endif
#ifndef Fonts
  #define Fonts      Root + "\build\fonts"
#endif

[Setup]
AppId={{1B918F3B-EA99-4DF2-A23E-06B081534B1C}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} (Windows 10 and 11 edition)
AppPublisher=Broderbund (2000); modernised for Windows 10 and 11
VersionInfoDescription={#AppName} Setup
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
MinVersion=10.0.17134
ArchitecturesAllowed=x86compatible
WizardStyle=modern
SetupIconFile={#Root}\launcher\kidpix_multi.ico
UninstallDisplayIcon={app}\Kid Pix Launcher.exe
UninstallDisplayName={#AppName}
OutputDir={#Root}\out
OutputBaseFilename=KidPixDeluxe3-Setup
Compression=lzma2/max
SolidCompression=no
LZMANumBlockThreads=4
LZMAUseSeparateProcess=yes
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop shortcut"; GroupDescription: "Shortcuts:"

[Files]
; --- the launcher (the shortcuts point here) -------------------------------
Source: "{#Game}\Kid Pix Launcher.exe"; DestDir: "{app}"; Flags: ignoreversion

; --- game program, patched -------------------------------------------------
; 13-byte MIDI patch
Source: "{#Game}\KPD3.exe";             DestDir: "{app}"; Flags: ignoreversion
; our compatibility shim
Source: "{#Game}\configit.dll"; DestDir: "{app}"; Flags: ignoreversion
; the original configit.dll
Source: "{#Game}\configit_real.dll";    DestDir: "{app}"; Flags: ignoreversion
; cnc-ddraw 7.1.0.0
Source: "{#Game}\ddraw.dll";            DestDir: "{app}"; Flags: ignoreversion
; 800x600 until the launcher sets a size; log=0. From the source tree, not
; the game folder, where the launcher may have changed it.
Source: "{#Root}\game\ddraw.ini";              DestDir: "{app}"; Flags: ignoreversion
Source: "{#Game}\Shaders\*";            DestDir: "{app}\Shaders"; Flags: ignoreversion recursesubdirs createallsubdirs
; Disc=1 -> 0 (no CD needed)
Source: "{#Game}\KPD3.cfg";             DestDir: "{app}"; Flags: ignoreversion
; full-install settings, no splash
Source: "{#Game}\KPD3.ini";               DestDir: "{app}"; Flags: ignoreversion
Source: "{#Game}\Mohawk.w32";           DestDir: "{app}"; Flags: ignoreversion
; The manual must NOT be called KdpxManual.pdf: Windows' built-in
; compatibility database recognises that name and forces an admin prompt.
Source: "{#Game}\KPD3Manual.pdf";       DestDir: "{app}"; Flags: ignoreversion
; configit_real.dll needs it
Source: "{#Game}\proident.pi";          DestDir: "{app}"; Flags: ignoreversion
; text-to-speech data
Source: "{#Game}\engphon.env";          DestDir: "{app}"; Flags: ignoreversion
Source: "{#Game}\usa86.lex";            DestDir: "{app}"; Flags: ignoreversion
Source: "{#Game}\Read Me.txt";          DestDir: "{app}"; Flags: ignoreversion
Source: "{#Game}\KidPixWeb.url";        DestDir: "{app}"; Flags: ignoreversion
Source: "{#Game}\*.ico";                DestDir: "{app}"; Flags: ignoreversion
Source: "Read Me - Windows 10 and 11.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Game}\cnc-ddraw LICENSE.txt"; DestDir: "{app}"; Flags: ignoreversion

; --- fonts: installed into Windows for this user (Windows 10 1803+). Not in
;     the game folder: the game registers any .ttf it finds there at every
;     start, which can take seconds per font. -----------------------------
Source: "{#Fonts}\CATCN___.TTF"; DestDir: "{autofonts}"; FontInstall: "Catchup Normal"; Flags: onlyifdoesntexist
Source: "{#Fonts}\CUCKOO.TTF"; DestDir: "{autofonts}"; FontInstall: "Cuckoo Regular"; Flags: onlyifdoesntexist
Source: "{#Fonts}\DIPLOMA.TTF"; DestDir: "{autofonts}"; FontInstall: "Diploma Regular"; Flags: onlyifdoesntexist
Source: "{#Fonts}\EURAN___.TTF"; DestDir: "{autofonts}"; FontInstall: "Eurasia Normal"; Flags: onlyifdoesntexist
Source: "{#Fonts}\EXPON___.TTF"; DestDir: "{autofonts}"; FontInstall: "Expose Normal"; Flags: onlyifdoesntexist
Source: "{#Fonts}\FIFTHAVE.TTF"; DestDir: "{autofonts}"; FontInstall: "FifthAve Regular"; Flags: onlyifdoesntexist
Source: "{#Fonts}\FLATBN__.TTF"; DestDir: "{autofonts}"; FontInstall: "Flat Brush Normal"; Flags: onlyifdoesntexist
Source: "{#Fonts}\FRANCISC.TTF"; DestDir: "{autofonts}"; FontInstall: "Franciscan Regular"; Flags: onlyifdoesntexist
Source: "{#Fonts}\GREMN___.TTF"; DestDir: "{autofonts}"; FontInstall: "Gremlin Normal"; Flags: onlyifdoesntexist
Source: "{#Fonts}\JESTER.TTF"; DestDir: "{autofonts}"; FontInstall: "Jester Regular"; Flags: onlyifdoesntexist
Source: "{#Fonts}\JOLTN___.TTF"; DestDir: "{autofonts}"; FontInstall: "Jolt Normal"; Flags: onlyifdoesntexist
Source: "{#Fonts}\KEIRACOR.TTF"; DestDir: "{autofonts}"; FontInstall: "Keira Condensed Normal"; Flags: onlyifdoesntexist
Source: "{#Fonts}\MARKET.TTF"; DestDir: "{autofonts}"; FontInstall: "Market Regular"; Flags: onlyifdoesntexist
Source: "{#Fonts}\PATRICK.TTF"; DestDir: "{autofonts}"; FontInstall: "Patrick Regular"; Flags: onlyifdoesntexist
Source: "{#Fonts}\PEGASUS.TTF"; DestDir: "{autofonts}"; FontInstall: "Pegasus Regular"; Flags: onlyifdoesntexist
Source: "{#Fonts}\SOCKET.TTF"; DestDir: "{autofonts}"; FontInstall: "Socket Regular"; Flags: onlyifdoesntexist
Source: "{#Fonts}\STORYBOO.TTF"; DestDir: "{autofonts}"; FontInstall: "Storybook Regular"; Flags: onlyifdoesntexist
Source: "{#Fonts}\TECHNN__.TTF"; DestDir: "{autofonts}"; FontInstall: "Techno Normal"; Flags: onlyifdoesntexist
Source: "{#Fonts}\TELETYPE.TTF"; DestDir: "{autofonts}"; FontInstall: "Teletype Regular"; Flags: onlyifdoesntexist
Source: "{#Fonts}\TUBULAR.TTF"; DestDir: "{autofonts}"; FontInstall: "Tubular Regular"; Flags: onlyifdoesntexist
Source: "{#Fonts}\Tabloid_.ttf"; DestDir: "{autofonts}"; FontInstall: "Tabloid"; Flags: onlyifdoesntexist
Source: "{#Fonts}\VOGEN___.TTF"; DestDir: "{autofonts}"; FontInstall: "Vogel Normal"; Flags: onlyifdoesntexist

; --- game data (unchanged from the CD) --------------------------------------
Source: "{#Game}\Data\*";               DestDir: "{app}\Data";         Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Game}\Libraries\*";          DestDir: "{app}\Libraries";    Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Game}\Idea Machine\*";       DestDir: "{app}\Idea Machine"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#Game}\QTmovies\*";           DestDir: "{app}\QTmovies";     Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\Kid Pix Launcher.exe"; WorkingDir: "{app}"; Comment: "Choose a window size and play Kid Pix Deluxe 3"; Tasks: desktopicon
Name: "{autoprograms}\{#AppName}\{#AppName}"; Filename: "{app}\Kid Pix Launcher.exe"; WorkingDir: "{app}"; Comment: "Choose a window size and play Kid Pix Deluxe 3"
Name: "{autoprograms}\{#AppName}\Kid Pix Deluxe 3 Manual"; Filename: "{app}\KPD3Manual.pdf"
Name: "{autoprograms}\{#AppName}\Read Me"; Filename: "{app}\Read Me.txt"
Name: "{autoprograms}\{#AppName}\Read Me - Windows 10 and 11"; Filename: "{app}\Read Me - Windows 10 and 11.txt"
Name: "{autoprograms}\{#AppName}\Uninstall {#AppName}"; Filename: "{uninstallexe}"

[Registry]
; The launcher remembers the chosen window size here; remove it on uninstall.
Root: HKCU; Subkey: "Software\KidPixDeluxe3"; Flags: uninsdeletekey

[Run]
Filename: "{app}\Kid Pix Launcher.exe"; WorkingDir: "{app}"; Description: "Start {#AppName} now"; Flags: postinstall nowait skipifsilent

[UninstallDelete]
; Created at run time. The children's pictures in {app}\Users are kept.
Type: files; Name: "{app}\configit.log"

[InstallDelete]
; Earlier builds put the fonts in the game folder.
Type: files; Name: "{app}\*.ttf"

[Messages]
FinishedLabel=Setup has installed [name] on your computer.%n%nUse the "Kid Pix Deluxe 3" shortcut to choose a window size and play. Each child's pictures are saved in the Users folder inside the game folder, and are kept if you uninstall.

[Code]
const
  WC_NO_BEST_FIT_CHARS = $400;
  CP_UTF8 = 65001;
  CompatStore = 'Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Compatibility Assistant\Store';
  CompatLayers = 'Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers';

function GetACP(): Cardinal;
  external 'GetACP@kernel32.dll stdcall';
function WideCharToMultiByte(CodePage, dwFlags: Cardinal; lpWideCharStr: String; cchWideChar: Integer;
  lpMultiByteStr: AnsiString; cbMultiByte: Integer; lpDefaultChar: Cardinal; var lpUsedDefaultChar: Integer): Integer;
  external 'WideCharToMultiByte@kernel32.dll stdcall';

{ KPD3.exe uses the ANSI file APIs: a folder name with characters outside the
  system's ANSI code page (for example Cyrillic letters on an English PC)
  turns into '?' for the game and it cannot find its files. }
function FitsAnsiCodePage(const S: String): Boolean;
var
  Buf: AnsiString;
  UsedDefault, N: Integer;
begin
  Result := True;
  if (S = '') or (GetACP() = CP_UTF8) then Exit;
  Buf := StringOfChar(' ', Length(S) * 4 + 4);
  UsedDefault := 0;
  N := WideCharToMultiByte(0, WC_NO_BEST_FIT_CHARS, S, Length(S), Buf, Length(Buf), 0, UsedDefault);
  Result := (N > 0) and (UsedDefault = 0);
end;

function IsUnder(const Dir, Parent: String): Boolean;
begin
  Result := (Parent <> '') and PathStartsWith(AddBackslash(Dir), AddBackslash(Parent), True);
end;

{ Program Files and Windows: KPD3.exe's writes there would be redirected to
  the VirtualStore while the launcher's would fail (see [Setup]). }
function IsProtectedLocation(const Dir: String): Boolean;
begin
  Result := IsUnder(Dir, ExpandConstant('{commonpf32}')) or IsUnder(Dir, ExpandConstant('{win}'));
  if not Result and IsWin64 then
    Result := IsUnder(Dir, ExpandConstant('{commonpf64}'));
end;

{ Tries to create a file in the nearest folder of Dir that already exists. }
function CanWriteTo(const Dir: String): Boolean;
var
  P, Parent, Probe: String;
begin
  P := RemoveBackslashUnlessRoot(Dir);
  while not DirExists(P) do
  begin
    Parent := ExtractFileDir(P);
    if (Parent = '') or (Parent = P) then Break;
    P := Parent;
  end;
  Probe := AddBackslash(P) + 'kpd3-setup-write-test.tmp';
  Result := SaveStringToFile(Probe, 'test', False);
  if Result then
    DeleteFile(Probe);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Dir: String;
begin
  Result := True;
  if CurPageID <> wpSelectDir then Exit;
  Dir := WizardDirValue;

  if IsProtectedLocation(Dir) then
  begin
    Log('Refused install folder (Program Files or Windows): ' + Dir);
    SuppressibleMsgBox('Kid Pix Deluxe 3 cannot be installed in Program Files or the Windows folder: ' +
      'the game keeps the children''s pictures and its settings in its own folder, which Windows does ' +
      'not let it write to there.' + #13#10#13#10 +
      'Choose a folder in your own user folder (the suggested one), or a folder such as C:\Kid Pix Deluxe 3.',
      mbError, MB_OK, IDOK);
    Result := False;
    Exit;
  end;

  if not FitsAnsiCodePage(Dir) then
  begin
    Log('Install folder has characters outside the ANSI code page: ' + Dir);
    if SuppressibleMsgBox('The folder name contains characters that Kid Pix Deluxe 3, a program from 2000, ' +
      'may not be able to use:' + #13#10#13#10 + Dir + #13#10#13#10 +
      'If the game does not start, install it in a folder such as C:\Kid Pix Deluxe 3 instead.' + #13#10#13#10 +
      'Install in this folder anyway?', mbConfirmation, MB_YESNO or MB_DEFBUTTON2, IDNO) <> IDYES then
    begin
      Result := False;
      Exit;
    end;
  end;

  if not CanWriteTo(Dir) then
  begin
    Log('Install folder is not writable: ' + Dir);
    SuppressibleMsgBox('You do not have permission to create files in this folder:' + #13#10#13#10 + Dir + #13#10#13#10 +
      'Choose a folder in your own user folder (the suggested one).', mbError, MB_OK, IDOK);
    Result := False;
    Exit;
  end;
  Log('Install folder accepted: ' + Dir);
end;

{ Silent installs skip the folder page, so NextButtonClick never checks the
  folder; repeat the two hard checks here (this runs in both modes). }
function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Dir: String;
begin
  Result := '';
  if not WizardSilent then Exit;
  Dir := ExpandConstant('{app}');
  if IsProtectedLocation(Dir) then
    Result := 'Kid Pix Deluxe 3 cannot be installed in Program Files or the Windows folder: ' + Dir
  else if not CanWriteTo(Dir) then
    Result := 'You do not have permission to create files in this folder: ' + Dir
  else if not FitsAnsiCodePage(Dir) then
    Log('Warning: the install folder has characters outside the ANSI code page: ' + Dir);
  if Result <> '' then Log('Refused install folder: ' + Result);
end;

// True if this copy of the game ({app}\KPD3.exe) is running; other copies do not matter.
function IsKidPixRunning(): Boolean;
var
  Locator, Service, Found, Proc: Variant;
  Exe, Path: String;
  I: Integer;
begin
  Result := False;
  Exe := ExpandConstant('{app}\KPD3.exe');
  try
    Locator := CreateOleObject('WbemScripting.SWbemLocator');
    Service := Locator.ConnectServer('.', 'root\CIMV2');
    Found := Service.ExecQuery('SELECT ProcessId, ExecutablePath FROM Win32_Process WHERE Name = ''KPD3.exe''');
    for I := 0 to Found.Count - 1 do
    begin
      Proc := Found.ItemIndex(I);
      if not VarIsNull(Proc.ExecutablePath) then
      begin
        Path := Proc.ExecutablePath;
        if PathSame(Path, Exe) then
        begin
          Result := True;
          Exit;
        end;
      end;
    end;
  except
    Log('Could not check for a running KPD3.exe: ' + GetExceptionMessage);
  end;
end;

function InitializeUninstall(): Boolean;
begin
  Result := True;
  while IsKidPixRunning() do
  begin
    Log('KPD3.exe is running');
    if SuppressibleMsgBox('Kid Pix Deluxe 3 is running. Close it, then click Retry.', mbError,
      MB_RETRYCANCEL, IDCANCEL) <> IDRETRY then
    begin
      Result := False;
      Exit;
    end;
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Exe, Kept: String;
begin
  if CurUninstallStep <> usPostUninstall then Exit;
  Exe := ExpandConstant('{app}\KPD3.exe');
  { Windows' compatibility records for this copy of the game, keyed by its path. }
  RegDeleteValue(HKCU, CompatStore, Exe);
  RegDeleteValue(HKCU, CompatLayers, Exe);
  // The Users folder was removed above if it was empty; anything left is the
  // children's pictures (and cnc-ddraw's Print Screen pictures).
  Kept := '';
  if DirExists(ExpandConstant('{app}\Users')) then
    Kept := Kept + #13#10 + '  ' + ExpandConstant('{app}\Users');
  if DirExists(ExpandConstant('{app}\Screenshots')) then
    Kept := Kept + #13#10 + '  ' + ExpandConstant('{app}\Screenshots');
  if Kept <> '' then
  begin
    Log('Kept user files:' + Kept);
    if not UninstallSilent then
      MsgBox('Kid Pix Deluxe 3 was removed. The pictures were kept in:' + #13#10 + Kept + #13#10#13#10 +
        'Delete that folder yourself if you do not want them.', mbInformation, MB_OK);
  end;
end;
