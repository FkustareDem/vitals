@echo off
setlocal
cd /d "%~dp0"

set "VSDIR="
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"

if not defined VSDIR set "VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" (
    echo [!] MSVC not found. Please edit VSDIR in this file.
    pause
    exit /b 1
)

call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo [!] vcvars64.bat failed.
    pause
    exit /b 1
)

if exist vitals.rc (rc /nologo vitals.rc)

cl /nologo /W3 /O2 /utf-8 vitals.c vitals.res ^
   /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup user32.lib gdi32.lib advapi32.lib dwmapi.lib

if errorlevel 1 (
    echo.
    echo [!] BUILD FAILED - send the errors above back to the developer.
    pause
    exit /b 1
)

del /q *.obj *.res *.exp *.lib >nul 2>nul
echo.
echo [OK] built: %~dp0vitals.exe
echo      double-click it to run; settings are in vitals.ini
echo.
pause
