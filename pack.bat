@echo off
setlocal
cd /d "%~dp0"
title vitals - release packer

set "VSDIR="
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR set "VSDIR=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" (
    echo [!] MSVC not found. Edit VSDIR in this file.
    pause
    exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul

echo [1/4] compiling resources ...
rc /nologo vitals.rc
if errorlevel 1 ( echo [!] rc failed & pause & exit /b 1 )

echo [2/4] compiling vitals.c ...
cl /nologo /W3 /O2 /utf-8 vitals.c vitals.res /link /SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup user32.lib gdi32.lib advapi32.lib shell32.lib dwmapi.lib
if errorlevel 1 ( echo [!] build failed & pause & exit /b 1 )

echo [3/4] assembling dist ...
if exist dist rmdir /s /q dist
mkdir "dist\vitals"
copy /y vitals.exe "dist\vitals\" >nul
if exist vitals.ini copy /y vitals.ini "dist\vitals\" >nul
if exist PROJECT.md copy /y PROJECT.md "dist\vitals\" >nul
if exist README.md  copy /y README.md  "dist\vitals\" >nul

echo [4/4] zipping ...
powershell -NoProfile -Command "Compress-Archive -Path 'dist\vitals\*' -DestinationPath 'dist\vitals-win64.zip' -Force"
if errorlevel 1 ( echo [!] zip failed & pause & exit /b 1 )

del /q *.obj *.res >nul 2>nul
echo.
echo [OK] dist\vitals-win64.zip   ^<- upload this file to GitHub Releases
echo.
pause
