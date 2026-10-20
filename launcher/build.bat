@echo off
rem Builds Kid Pix Launcher.exe into build\ with the C# compiler that comes
rem with Windows (.NET Framework 4.x); nothing else to install.
setlocal
cd /d "%~dp0"
set "CSC=%WINDIR%\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
if not exist "%CSC%" set "CSC=%WINDIR%\Microsoft.NET\Framework\v4.0.30319\csc.exe"
if not exist "%CSC%" (
    echo .NET Framework 4 C# compiler not found.
    exit /b 1
)
if not exist build mkdir build
"%CSC%" /nologo /target:winexe /optimize+ /platform:anycpu /win32icon:kidpix_multi.ico /out:"build\Kid Pix Launcher.exe" /r:System.Windows.Forms.dll /r:System.Drawing.dll KidPixLauncher.cs
