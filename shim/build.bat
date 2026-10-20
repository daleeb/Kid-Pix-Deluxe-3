@echo off
rem Builds configit.dll, the Kid Pix Deluxe 3 compatibility shim (32-bit).
rem
rem   build.bat            build into build\configit.dll
rem   build.bat install    build, then copy it into the prepared game,
rem                        ..uild\game\configit.dll (see ..\prepare.ps1)
rem
rem Needs Visual Studio 2022 (or newer) with the C++ desktop workload.
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
if exist "%VSWHERE%" for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR set "VSDIR=%ProgramFiles%\Microsoft Visual Studio\2022\Community"
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" (
    echo Visual Studio C++ tools not found. Install the "Desktop development with C++" workload.
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul 2>&1

if not exist build mkdir build
cl /nologo /LD /O2 /W3 /D_CRT_SECURE_NO_WARNINGS proxy.c /Fo:build\ /Fe:build\configit.dll /link /DEF:proxy.def user32.lib kernel32.lib gdi32.lib ole32.lib oleaut32.lib shell32.lib
if errorlevel 1 exit /b 1

if /i not "%~1"=="install" exit /b 0
set "PLAY=%~dp0..uild\game"
tasklist /fi "imagename eq KPD3.exe" 2>nul | find /i "KPD3.exe" >nul && (
    echo Kid Pix is running - close it first.
    exit /b 1
)
if not exist "%PLAY%\KPD3.exe" (
    echo No prepared game in %PLAY% - run prepare.ps1 first.
    exit /b 1
)
copy /y build\configit.dll "%PLAY%\configit.dll" >nul || exit /b 1
echo Installed to %PLAY%\configit.dll
