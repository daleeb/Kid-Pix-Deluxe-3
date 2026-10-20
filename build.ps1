<#
.SYNOPSIS
  Builds everything: the shim, the launcher, the patched game, the installer
  and the ISO.

.EXAMPLE
  .\build.ps1 -Cd E:\          from the CD in drive E:
  .\build.ps1                  reuse build\game from an earlier run
  .\build.ps1 -Cd E:\ -NoIso   installer only

  Output: out\KidPixDeluxe3-Setup.exe, out\KidPixDeluxe3.iso (+ .sha256)

  Needs Visual Studio 2022+ with the C++ workload (for the shim) and
  Inno Setup 6 (ISCC.exe). The launcher uses the C# compiler in Windows.
#>
param(
    [string]$Cd,
    [switch]$NoIso
)
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
Set-Location $here
[Environment]::CurrentDirectory = $here      # .NET path calls resolve against this, not Set-Location

$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
          "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
          "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 (ISCC.exe) not found.' }

Write-Host '== Shim'
& cmd /c "`"$here\shim\build.bat`""
if ($LASTEXITCODE) { throw 'Shim build failed' }
Write-Host '== Launcher'
& cmd /c "`"$here\launcher\build.bat`""
if ($LASTEXITCODE) { throw 'Launcher build failed' }

Write-Host '== Game'
if ($Cd) {
    & (Join-Path $here 'prepare.ps1') -Cd $Cd
} elseif (Test-Path (Join-Path $here 'build\game\KPD3.exe')) {
    # Refresh the parts built above in the existing game folder.
    Copy-Item 'shim\build\configit.dll' 'build\game\configit.dll'
    Copy-Item 'launcher\build\Kid Pix Launcher.exe' 'build\game'
    Write-Host 'Reusing build\game (pass -Cd to rebuild it from the CD).'
} else {
    throw 'No prepared game yet: pass -Cd <your Kid Pix Deluxe 3 CD>.'
}

Write-Host '== Installer'
& $iscc /Q (Join-Path $here 'installer\KidPixDeluxe3.iss')
if ($LASTEXITCODE) { throw "ISCC failed ($LASTEXITCODE)" }
$setup = Get-Item (Join-Path $here 'out\KidPixDeluxe3-Setup.exe')
"Installer: {0} ({1:N1} MB)" -f $setup.FullName, ($setup.Length / 1MB)
if ($NoIso) { return }

Write-Host '== ISO'
# The disc: the installer as Setup.exe, an autorun that offers it, the read-me.
$stage = Join-Path $env:TEMP 'KidPixDeluxe3-iso'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory $stage | Out-Null
Copy-Item $setup.FullName (Join-Path $stage 'Setup.exe')
Copy-Item 'installer\iso\AUTORUN.INF', 'installer\iso\Autorun.ico' $stage
Copy-Item 'installer\Read Me - Windows 10 and 11.txt' $stage

. (Join-Path $here 'installer\tools\New-IsoFromFolder.ps1')
$isoPath = Join-Path $here 'out\KidPixDeluxe3.iso'
$iso = New-IsoFromFolder -SourceFolder $stage -IsoPath $isoPath -VolumeName 'Kid Pix Deluxe 3' -Force
Remove-Item $stage -Recurse -Force
$hash = (Get-FileHash $iso.IsoPath -Algorithm SHA256).Hash
"ISO:       {0} ({1:N1} MB)" -f $iso.IsoPath, ($iso.SizeBytes / 1MB)
"SHA-256:   $hash"
Set-Content -Path "$isoPath.sha256" -Value "$hash  KidPixDeluxe3.iso" -Encoding ascii
