<#
.SYNOPSIS
  Builds the patched Kid Pix Deluxe 3 game folder from your own CD.

.DESCRIPTION
  The game's files are Broderbund's and are not in this repository. This script
  copies them from a Kid Pix Deluxe 3 CD (or a copy of its contents), applies
  the Windows 10/11 changes, and checks every changed file against a known
  SHA-256 so a different CD release is caught instead of patched blindly.

  Supported CDs:
    US                     Kid Pix Deluxe 3 (2000), KPD3.exe 1.0
    Brazilian Portuguese   Kid Pix Deluxe 3 (2003), KPD3.exe 1.0.1

  Output (default .\build):
    build\game\    the patched game, ready to run or to package
    build\fonts\   the CD's 22 fonts (the installer installs them per user)

  The shim (shim\build.bat) must be built first; this copies
  shim\build\configit.dll into the game folder.

.EXAMPLE
  .\prepare.ps1 -Cd E:\
  .\prepare.ps1 -Cd 'D:\Kid Pix Deluxe 3 CD' -Out D:\kp3build
#>
param(
    [Parameter(Mandatory)][string]$Cd,
    [string]$Out = (Join-Path $PSScriptRoot 'build')
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
[Environment]::CurrentDirectory = $here

# What each CD's files hash to, before and after the changes below. The
# 13-byte MIDI patch is the same in both builds, at a different offset.
$editions = @(
    @{ Name = 'US'; Exe = 'ab8cac41806d205298be92de0693b09cb167715ad8bcae9cc4bb5d4a224cf518'
       ExePatched = '25a54f06286f64966e91b258fac2043e80bf958b245a5e880526a96b770edc58'; MidiAt = 0xE04E2
       Cfg = '5bde94919458f83166dcec0658d69f5176122757df89a706aed4c333fff7d918'
       CfgPatched = '41a5d30778f007f8c92261e6e49a6a3e1354d32a003f76f90eb4af103af1b31f'
       Ini = '9864eeef98a365e698bb453752644541b0afcc32f2ce77f581f8cde194966627'
       IniPatched = '7f19f33b71ea228c4a2fda44a8d221fcd2f3d29fd52b59a73975e367c1b02b9d' },
    @{ Name = 'Brazilian Portuguese'; Exe = '4c0a69d7ba5038e9c4c082a4ea74525801f181b47728ba464a1f9c80344f480e'
       ExePatched = '6ce6fb12b3b1807ddc55ae5845ade2276e0a90bdc295b609fe762fc1c71d1fe9'; MidiAt = 0xE13B2
       Cfg = '870b611b53eeb65952d59b394936a0cee5747a5bdad2cdc34e6d64a2779df8ac'
       CfgPatched = 'e602b98c6a139a39892ae078d22b5322c1c11b1c94b0e47f11fead5d197947b9'
       Ini = 'affd72ab61356b66bc38aef1a6aab6cefb71c8d7279e54ee89be9f067ee01196'
       IniPatched = '0e714a3543082e3ebee70ce7b23e0a554f86f0e2992e84a75923c47fabda2bd9' }
)

function Sha([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Expect([string]$Path, [string]$Hash, [string]$What) {
    $h = Sha $Path
    if ($h -ne $Hash) { throw "$What has an unexpected SHA-256 ($h)." }
}
# Replace exactly one occurrence of a byte sequence (as Latin-1 text, so CRLFs and tabs are kept).
function Edit-Text([string]$Path, [string]$Old, [string]$New) {
    $enc = [Text.Encoding]::GetEncoding(28591)
    $t = $enc.GetString([IO.File]::ReadAllBytes($Path))
    $i = $t.IndexOf($Old, [StringComparison]::Ordinal)
    if ($i -lt 0 -or $t.IndexOf($Old, $i + 1, [StringComparison]::Ordinal) -ge 0) { throw "Expected exactly one '$Old' in $Path" }
    [IO.File]::WriteAllBytes($Path, $enc.GetBytes($t.Substring(0, $i) + $New + $t.Substring($i + $Old.Length)))
}

$Cd = (Resolve-Path -LiteralPath $Cd).ProviderPath
foreach ($p in 'KpInstall\KPD3.exe', 'INSTALL\Data\KPD3.ini', 'INSTALL\Fonts', 'Data', 'Libraries', 'Idea Machine') {
    if (-not (Test-Path -LiteralPath (Join-Path $Cd $p))) { throw "Not a Kid Pix Deluxe 3 CD: $(Join-Path $Cd $p) is missing." }
}
$cdExe = Sha (Join-Path $Cd 'KpInstall\KPD3.exe')
$ed = $editions | Where-Object { $_.Exe -eq $cdExe }
if (-not $ed) { throw "KpInstall\KPD3.exe ($cdExe) is from a Kid Pix Deluxe 3 CD this script does not know. Supported: US, Brazilian Portuguese." }
Write-Host "Kid Pix Deluxe 3 CD: $($ed.Name) edition"
$shim = Join-Path $here 'shim\build\configit.dll'
if (-not (Test-Path $shim)) { throw "Build the shim first: shim\build.bat" }

$game  = Join-Path $Out 'game'
$fonts = Join-Path $Out 'fonts'
if (Test-Path $game)  { Remove-Item -LiteralPath $game  -Recurse -Force }
if (Test-Path $fonts) { Remove-Item -LiteralPath $fonts -Recurse -Force }
New-Item -ItemType Directory $game, $fonts | Out-Null

Write-Host 'Copying the game from the CD...'
# The files a full install copies (KpInstall), except the three handled below.
# Most of them are hidden on the US CD, hence -Force. KPRUN.EXE (Brazilian CD)
# is left out: Windows' compatibility database flags a Broderbund KPRUN.EXE
# next to the game.
Get-ChildItem -LiteralPath (Join-Path $Cd 'KpInstall') -File -Force |
    Where-Object { $_.Name -notin 'KdpxManual.pdf', 'configit.dll', 'KPD3.ini', 'KPRUN.EXE', 'KPRUN.INI' } |
    Copy-Item -Destination $game
# The Brazilian KPD3.exe checks for its CD at every start, through the
# FileCheck= line of kprun.ini; the launcher points it at the CD's drive.
$kprun = Join-Path $Cd 'KpInstall\KPRUN.INI'
if (Test-Path -LiteralPath $kprun) { Copy-Item -LiteralPath $kprun -Destination (Join-Path $game 'kprun.ini') }
# Content folders from the CD root, so the game never asks for the CD. The
# Brazilian CD keeps the game data one level down, in Data\Data, and has no
# QTmovies (which needs QuickTime anyway).
$data = Join-Path $Cd 'Data'
if (Test-Path -LiteralPath (Join-Path $data 'Data\General.mhk')) { $data = Join-Path $data 'Data' }
Copy-Item -LiteralPath $data -Destination (Join-Path $game 'Data') -Recurse
foreach ($d in 'Libraries', 'Idea Machine', 'QTmovies') {
    if (Test-Path -LiteralPath (Join-Path $Cd $d)) { Copy-Item -LiteralPath (Join-Path $Cd $d) -Destination (Join-Path $game $d) -Recurse }
}
Get-ChildItem -LiteralPath (Join-Path $Cd 'INSTALL\Fonts') -File -Force | Copy-Item -Destination $fonts
Copy-Item -LiteralPath (Join-Path $Cd 'INSTALL\Data\KPD3.ini') -Destination (Join-Path $game 'KPD3.ini')
Copy-Item -LiteralPath (Join-Path $Cd 'KpInstall\KdpxManual.pdf') -Destination (Join-Path $game 'KPD3Manual.pdf')
Copy-Item -LiteralPath (Join-Path $Cd 'KpInstall\configit.dll') -Destination (Join-Path $game 'configit_real.dll') -Force
# Files from a CD are read-only, and the CD hides several: make them all normal.
Get-ChildItem -LiteralPath $game, $fonts -Recurse -Force -File | ForEach-Object { $_.Attributes = 'Normal' }

Write-Host 'Applying the changes...'
# 1. KPD3.exe: 13 bytes make the MIDI device query (which crashes on modern
#    Windows) return an empty device at once. MIDI is off anyway.
$exe = Join-Path $game 'KPD3.exe'
$b = [IO.File]::ReadAllBytes($exe)
$old = [byte[]](0x55,0x8B,0xEC,0x51,0x56,0xC7,0x45,0xFC,0xCC,0xCC,0xCC,0xCC,0x8B)
$new = [byte[]](0x8B,0x44,0x24,0x08,0xC6,0x40,0x08,0x00,0x33,0xC0,0xC2,0x0C,0x00)
for ($i = 0; $i -lt 13; $i++) { if ($b[$ed.MidiAt + $i] -ne $old[$i]) { throw 'KPD3.exe: patch site not as expected' }; $b[$ed.MidiAt + $i] = $new[$i] }
[IO.File]::WriteAllBytes($exe, $b)
Expect $exe $ed.ExePatched 'patched KPD3.exe'

# 2. KPD3.cfg: three asset groups were marked as on the CD (Disc = 1); they
#    are copied into Data, so mark them local or the game asks for the CD.
$cfg = Join-Path $game 'KPD3.cfg'
Expect $cfg $ed.Cfg 'KpInstall\KPD3.cfg'
foreach ($sec in 'Splash', 'RHelpDlg', 'RSprayPanel') {
    $enc = [Text.Encoding]::GetEncoding(28591)
    $t = $enc.GetString([IO.File]::ReadAllBytes($cfg))
    $s = $t.IndexOf("[$sec]`r`n", [StringComparison]::Ordinal)
    $d = $t.IndexOf("Disc = 1`r`n", $s, [StringComparison]::Ordinal)
    if ($s -lt 0 -or $d -lt 0 -or $t.IndexOf("`r`n[", $s + 1, [StringComparison]::Ordinal) -lt $d) { throw "KPD3.cfg: no 'Disc = 1' in [$sec]" }
    [IO.File]::WriteAllBytes($cfg, $enc.GetBytes($t.Substring(0, $d) + 'Disc = 0' + $t.Substring($d + 8)))
}
Expect $cfg $ed.CfgPatched 'patched KPD3.cfg'

# 3. Mohawk.w32: run in a window; cnc-ddraw does the scaling.
$w32 = Join-Path $game 'Mohawk.w32'
Expect $w32 '8003ce76135f62090046044d343ace2eb6d97ba1f75bb374650bbb4960a268ba' 'KpInstall\Mohawk.w32'
Edit-Text $w32 "[DirectDraw]`r`nfEnable=true`r`n" "[DirectDraw]`r`nfEnable=true`r`nfFullScreen=false`r`n"
Expect $w32 '803ae41d9c7912cadae3db5b0a0947d03f86bfe6153219a98c81ce4ed0a5d62a' 'patched Mohawk.w32'

# 4. KPD3.ini: the full-install version from INSTALL\Data (no CD needed),
#    without the splash screen and without Broderbund's leftover test name.
$ini = Join-Path $game 'KPD3.ini'
Expect $ini $ed.Ini 'INSTALL\Data\KPD3.ini'
Edit-Text $ini "ShowSplashScreen`t`t= `"true`"" "ShowSplashScreen`t`t= `"false`""
Edit-Text $ini "AutoLogin`t`t`t`t=  `"jojoberry`"" "AutoLogin`t`t`t`t=  `"`""
Expect $ini $ed.IniPatched 'patched KPD3.ini'

# 5. The manual under a new name: Windows' compatibility database recognises
#    KdpxManual.pdf and then forces the game to run as administrator.
#    (Copied above as KPD3Manual.pdf.)

# 6. The shim takes configit.dll's place; the original becomes configit_real.dll.
Expect (Join-Path $game 'configit_real.dll') 'd66a907f58356ba81dcea425c938152bcd18d1f5281e527446a821697d9255fc' 'KpInstall\configit.dll'
Copy-Item -LiteralPath $shim -Destination (Join-Path $game 'configit.dll')

# 7. cnc-ddraw (scaling) and its settings.
Copy-Item (Join-Path $here 'third_party\cnc-ddraw\ddraw.dll') $game
Copy-Item (Join-Path $here 'third_party\cnc-ddraw\Shaders') (Join-Path $game 'Shaders') -Recurse
Copy-Item (Join-Path $here 'third_party\cnc-ddraw\LICENSE.txt') (Join-Path $game 'cnc-ddraw LICENSE.txt')
Copy-Item (Join-Path $here 'game\ddraw.ini') $game

# 8. The launcher, if it has been built.
$launcher = Join-Path $here 'launcher\build\Kid Pix Launcher.exe'
if (Test-Path $launcher) { Copy-Item $launcher $game } else { Write-Warning 'Launcher not built (launcher\build.bat); the game folder has no launcher.' }

Write-Host "Done: $game"
Write-Host "Play: run 'Kid Pix Launcher.exe' in that folder, or build the installer with build.ps1."
