<#
.SYNOPSIS
  Builds the patched Kid Pix Deluxe 3 game folder from your own CD.

.DESCRIPTION
  The game's files are Broderbund's and are not in this repository. This script
  copies them from a Kid Pix Deluxe 3 CD (or a copy of its contents), applies
  the Windows 10/11 changes, and checks every changed file against a known
  SHA-256 so a different CD release is caught instead of patched blindly.

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

function Sha([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Expect([string]$Path, [string]$Hash, [string]$What) {
    $h = Sha $Path
    if ($h -ne $Hash) { throw "$What has an unexpected SHA-256 ($h). This script was written for the original Kid Pix Deluxe 3 (2000) CD." }
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
foreach ($p in 'KpInstall\KPD3.exe', 'INSTALL\Data\KPD3.ini', 'INSTALL\Fonts', 'Data', 'Libraries', 'Idea Machine', 'QTmovies') {
    if (-not (Test-Path -LiteralPath (Join-Path $Cd $p))) { throw "Not a Kid Pix Deluxe 3 CD: $(Join-Path $Cd $p) is missing." }
}
$shim = Join-Path $here 'shim\build\configit.dll'
if (-not (Test-Path $shim)) { throw "Build the shim first: shim\build.bat" }

$game  = Join-Path $Out 'game'
$fonts = Join-Path $Out 'fonts'
if (Test-Path $game)  { Remove-Item -LiteralPath $game  -Recurse -Force }
if (Test-Path $fonts) { Remove-Item -LiteralPath $fonts -Recurse -Force }
New-Item -ItemType Directory $game, $fonts | Out-Null

Write-Host 'Copying the game from the CD...'
# The files a full install copies (KpInstall), except the three handled below.
# Most of them are hidden on the CD, hence -Force.
Get-ChildItem -LiteralPath (Join-Path $Cd 'KpInstall') -File -Force |
    Where-Object { $_.Name -notin 'KdpxManual.pdf', 'configit.dll', 'KPD3.ini' } |
    Copy-Item -Destination $game
# Content folders from the CD root, so the game never asks for the CD.
foreach ($d in 'Data', 'Libraries', 'Idea Machine', 'QTmovies') {
    Copy-Item -LiteralPath (Join-Path $Cd $d) -Destination (Join-Path $game $d) -Recurse
}
Get-ChildItem -LiteralPath (Join-Path $Cd 'INSTALL\Fonts') -File -Force | Copy-Item -Destination $fonts
Copy-Item -LiteralPath (Join-Path $Cd 'INSTALL\Data\KPD3.ini') -Destination (Join-Path $game 'KPD3.ini')
Copy-Item -LiteralPath (Join-Path $Cd 'KpInstall\KdpxManual.pdf') -Destination (Join-Path $game 'KPD3Manual.pdf')
Copy-Item -LiteralPath (Join-Path $Cd 'KpInstall\configit.dll') -Destination (Join-Path $game 'configit_real.dll') -Force
# Files from a CD are read-only, and the CD hides several: make them all normal.
Get-ChildItem -LiteralPath $game, $fonts -Recurse -Force -File | ForEach-Object { $_.Attributes = 'Normal' }

Write-Host 'Applying the changes...'
# 1. KPD3.exe: 13 bytes at 0xE04E2 make the MIDI device query (which crashes
#    on modern Windows) return an empty device at once. MIDI is off anyway.
$exe = Join-Path $game 'KPD3.exe'
Expect $exe 'ab8cac41806d205298be92de0693b09cb167715ad8bcae9cc4bb5d4a224cf518' 'KpInstall\KPD3.exe'
$b = [IO.File]::ReadAllBytes($exe)
$old = [byte[]](0x55,0x8B,0xEC,0x51,0x56,0xC7,0x45,0xFC,0xCC,0xCC,0xCC,0xCC,0x8B)
$new = [byte[]](0x8B,0x44,0x24,0x08,0xC6,0x40,0x08,0x00,0x33,0xC0,0xC2,0x0C,0x00)
for ($i = 0; $i -lt 13; $i++) { if ($b[0xE04E2 + $i] -ne $old[$i]) { throw 'KPD3.exe: patch site not as expected' }; $b[0xE04E2 + $i] = $new[$i] }
[IO.File]::WriteAllBytes($exe, $b)
Expect $exe '25a54f06286f64966e91b258fac2043e80bf958b245a5e880526a96b770edc58' 'patched KPD3.exe'

# 2. KPD3.cfg: three asset groups were marked as on the CD (Disc = 1); they
#    are copied into Data, so mark them local or the game asks for the CD.
$cfg = Join-Path $game 'KPD3.cfg'
Expect $cfg '5bde94919458f83166dcec0658d69f5176122757df89a706aed4c333fff7d918' 'KpInstall\KPD3.cfg'
foreach ($sec in 'Splash', 'RHelpDlg', 'RSprayPanel') {
    $enc = [Text.Encoding]::GetEncoding(28591)
    $t = $enc.GetString([IO.File]::ReadAllBytes($cfg))
    $s = $t.IndexOf("[$sec]`r`n", [StringComparison]::Ordinal)
    $d = $t.IndexOf("Disc = 1`r`n", $s, [StringComparison]::Ordinal)
    if ($s -lt 0 -or $d -lt 0 -or $t.IndexOf("`r`n[", $s + 1, [StringComparison]::Ordinal) -lt $d) { throw "KPD3.cfg: no 'Disc = 1' in [$sec]" }
    [IO.File]::WriteAllBytes($cfg, $enc.GetBytes($t.Substring(0, $d) + 'Disc = 0' + $t.Substring($d + 8)))
}
Expect $cfg '41a5d30778f007f8c92261e6e49a6a3e1354d32a003f76f90eb4af103af1b31f' 'patched KPD3.cfg'

# 3. Mohawk.w32: run in a window; cnc-ddraw does the scaling.
$w32 = Join-Path $game 'Mohawk.w32'
Expect $w32 '8003ce76135f62090046044d343ace2eb6d97ba1f75bb374650bbb4960a268ba' 'KpInstall\Mohawk.w32'
Edit-Text $w32 "[DirectDraw]`r`nfEnable=true`r`n" "[DirectDraw]`r`nfEnable=true`r`nfFullScreen=false`r`n"
Expect $w32 '803ae41d9c7912cadae3db5b0a0947d03f86bfe6153219a98c81ce4ed0a5d62a' 'patched Mohawk.w32'

# 4. KPD3.ini: the full-install version from INSTALL\Data (no CD needed),
#    without the splash screen and without Broderbund's leftover test name.
$ini = Join-Path $game 'KPD3.ini'
Expect $ini '9864eeef98a365e698bb453752644541b0afcc32f2ce77f581f8cde194966627' 'INSTALL\Data\KPD3.ini'
Edit-Text $ini "ShowSplashScreen`t`t= `"true`"" "ShowSplashScreen`t`t= `"false`""
Edit-Text $ini "AutoLogin`t`t`t`t=  `"jojoberry`"" "AutoLogin`t`t`t`t=  `"`""
Expect $ini '7f19f33b71ea228c4a2fda44a8d221fcd2f3d29fd52b59a73975e367c1b02b9d' 'patched KPD3.ini'

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
