# Kid Pix Deluxe 3 for Windows 10 and 11

Kid Pix Deluxe 3 (Broderbund, 2000) can't be installed on modern Windows: its
setup program is 16-bit, which 64-bit Windows can't run. Even installed by
hand, the game crashes on multi-core CPUs, asks for administrator rights,
needs QuickTime 4 to save pictures, and runs at a fixed 800 × 600.

This repository turns your own Kid Pix Deluxe 3 CD into a normal Windows 10/11
installer (and an ISO that carries it). The installed game:

- runs scaled up to a modern screen, in a window or fullscreen, chosen in a
  small launcher (Original 800 × 600, Large 1.5×, Extra large 2×, Fullscreen);
- installs per user with no administrator prompt, and runs without one;
- saves and opens pictures **without QuickTime**;
- doesn't ask for the CD (US edition; see below for the Brazilian one).

**No game files are included.** Kid Pix is Broderbund's (now Houghton Mifflin
Harcourt's); you need the original CD. The build checks every game file it
changes against a known SHA-256 and stops if your CD differs.

### Supported CDs

| Edition | KPD3.exe | Notes |
|---|---|---|
| US (2000) | 1.0 | |
| Brazilian Portuguese (2003) | 1.0.1 | a different build: the shim has its own address table for it (`g_builds` in `shim\proxy.c`). The data is in `Data\Data` on this CD and there is no QTmovies folder; `prepare.ps1` handles both. This edition checks for its CD at every start (through `kprun.ini`), as it always did, so the CD or its ISO must be in a drive to play; the launcher finds it and points `kprun.ini` at that drive. |

Other releases are refused by `prepare.ps1`. Adding one means mapping the
shim's patch sites in its KPD3.exe and adding a row to `g_builds` and to
`$editions` in `prepare.ps1`.

## What's here

```
prepare.ps1          CD -> patched game folder (build\game) and fonts (build\fonts)
build.ps1            everything: shim, launcher, prepare, installer, ISO
shim\                configit.dll, the compatibility shim (C)
launcher\            Kid Pix Launcher.exe, the size chooser (C#)
game\ddraw.ini       cnc-ddraw and shim settings as shipped
installer\           Inno Setup script, end-user read-me, ISO autorun, ISO builder
third_party\         cnc-ddraw 7.1.0.0 (MIT)
```

## Building

Needs:

- your Kid Pix Deluxe 3 CD (or a copy of its contents);
- Visual Studio 2022 or newer with **Desktop development with C++** (for the shim);
- [Inno Setup 6](https://jrsoftware.org/isinfo.php);
- the launcher builds with the C# compiler that comes with Windows.

```powershell
.\build.ps1 -Cd E:\
```

Output:

```
out\KidPixDeluxe3-Setup.exe     the installer (about 240 MB)
out\KidPixDeluxe3.iso           a disc with Setup.exe, an autorun and the read-me
out\KidPixDeluxe3.iso.sha256
```

Later builds can skip `-Cd` and reuse `build\game`. `-NoIso` builds only the
installer. To play without installing, run `build\game\Kid Pix Launcher.exe`
(the fonts in `build\fonts` are then not installed, so text uses fallbacks).

The pieces separately:

```bat
shim\build.bat            builds shim\build\configit.dll
shim\build.bat install    ...and copies it into build\game
launcher\build.bat        builds launcher\build\Kid Pix Launcher.exe
```

```powershell
.\prepare.ps1 -Cd E:\ [-Out D:\somewhere]
```

The installer isn't code-signed, so a downloaded copy shows SmartScreen's
"Windows protected your PC" (More info > Run anyway), or unblock the ISO in
its Properties first.

## What gets changed

### From the CD (`prepare.ps1`)

The game is assembled the way a full install would have been: the files from
`KpInstall\`, the content folders `Data`, `Libraries`, `Idea Machine` and
`QTmovies` from the CD root (so the CD is never needed), and `KPD3.ini` from
`INSTALL\Data\` (the full-install version). Then:

| File | Change | Why |
|---|---|---|
| `KPD3.exe` | 13 bytes at file offset `0xE04E2` | the game's MIDI device query crashes on modern Windows; it now returns an empty device at once (MIDI is already off in `Mohawk.w32`) |
| `KPD3.cfg` | `Disc = 1` → `Disc = 0` in `[Splash]`, `[RHelpDlg]`, `[RSprayPanel]` | those assets were marked as on the CD; without this the game asks for the CD |
| `KPD3.ini` | `ShowSplashScreen = "false"`, `AutoLogin = ""` | no splash screen; clears a name Broderbund left in |
| `Mohawk.w32` | `fFullScreen=false` under `[DirectDraw]` | run in a window; cnc-ddraw does the scaling |
| `KdpxManual.pdf` | renamed `KPD3Manual.pdf` | Windows' compatibility database recognises the game by this file and applies its Windows 98 layer, which forces an administrator prompt |
| `configit.dll` | renamed `configit_real.dll`; the shim takes its place | see below |

The patch is:

```
8B 44 24 08     mov  eax, [esp+8]     ; the caller's MIDIOUTCAPS
C6 40 08 00     mov  byte [eax+8], 0  ; device name = ""
33 C0           xor  eax, eax         ; MMSYSERR_NOERROR
C2 0C 00        ret  0Ch
```

### cnc-ddraw (`ddraw.dll`, `ddraw.ini`)

[cnc-ddraw](https://github.com/FunkyFr3sh/cnc-ddraw) replaces DirectDraw for
this game only (Windows loads the `ddraw.dll` next to the exe first) and draws
the fixed 800 × 600, 16-bit picture scaled into a normal window. It uses the
GDI renderer, because the game makes its own OpenGL context on the same
window. The launcher writes the size keys into `ddraw.ini` before each start.

### The shim (`shim\`)

`KPD3.exe` loads `configit.dll` from its folder before any game code runs. The
shim passes the original's ten functions through to `configit_real.dll` and
fixes the running game in memory:

| Fix | What goes wrong without it |
|---|---|
| One CPU | a startup worker thread races the main thread on multi-core CPUs: random crashes, "R6025 pure virtual function call" |
| Screen size | the engine sizes its window from the desktop; the shim reports 800 × 600 |
| Sound check | a startup audio query fails on modern Windows and the game quits silently |
| MIDI check | Windows' MIDI query crashes inside the game |
| Display mode | stops the game switching the desktop to 800 × 600 × 16 |
| DirectDraw mode | keeps the mode at exactly 800 × 600, otherwise the screen stays black |
| Primary surface | creates the surface cnc-ddraw scales from, otherwise only a corner shows |
| Menu bar | the game's hidden menu strip is put across the top of the scaled picture and kept shown; Alt/F10 work |
| Manual | Help > View User Guide opens `KPD3Manual.pdf` in the default PDF viewer instead of looking for Acrobat 4 or Broderbund's missing helper |
| Fonts | the game's font-change broadcast waits on every window and could freeze startup; the shim posts it instead |
| Typing | arrows, Delete, Home/End and F-keys typed junk characters in the text tool |
| Drive scan | an unchecked removable-drive query could read garbage |
| No ghosting | Windows doesn't replace the window with a "Not responding" copy during long loads |
| QuickTime | a stand-in for the QuickTime calls the game makes; `.kpx` pictures store a JPEG, which is now encoded and decoded with Windows' own WIC |

Options go in a `[configit]` section of `ddraw.ini`:

```ini
[configit]
log=0          ; 0 = off, 1 = startup and errors, 2 = detailed (writes configit.log)
menubar=pinned ; pinned, auto (shown on Alt or a click at the top), off
```

### The installer (`installer\`)

- per user, to `%LOCALAPPDATA%\Programs\Kid Pix Deluxe 3`, no administrator
  rights (the game keeps pictures in its own folder, so it must be writable);
- installs the CD's 22 fonts for the current user (not in the game folder,
  where the game re-registers them at every start, which takes seconds);
- refuses Program Files and unwritable folders, warns about folder names the
  2000-era game can't handle;
- desktop and Start menu shortcuts start the launcher;
- uninstall keeps the children's pictures and removes the fonts and Windows'
  compatibility records for the game.

## What doesn't work

Without QuickTime: movies, animated toons, and importing pictures made outside
Kid Pix. Text-to-speech needs a 2000-era speech engine that isn't installed.
The first click after drawing or using a menu is sometimes ignored.

## Licence

cnc-ddraw is MIT-licensed (`third_party\cnc-ddraw\LICENSE.txt`). Kid Pix
Deluxe 3 and its files belong to their owners and are not part of this
repository.
