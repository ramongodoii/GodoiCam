@echo off
rem Compila a camera virtual (GodoiCanVCam.dll) e o app (GodoiCan.exe) em windows\bin.
setlocal
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" set "VSWHERE=C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS="
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\godoican_vs.txt"
set /p VS=<"%TEMP%\godoican_vs.txt"
if not defined VS (
    echo Visual Studio com C++ nao encontrado.
    exit /b 1
)
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if not exist bin mkdir bin
if not exist obj\vcam mkdir obj\vcam
if not exist obj\app mkdir obj\app

set CFLAGS=/nologo /std:c++17 /O2 /EHsc /W3 /MT /utf-8 /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DNOMINMAX /Icommon

echo [1/2] GodoiCanVCam.dll
cl %CFLAGS% /LD vcam\*.cpp /Fo"obj\vcam\\" /Fe"bin\GodoiCanVCam.dll" ^
   /link /DEF:vcam\GodoiCanVCam.def mfplat.lib mfuuid.lib ole32.lib advapi32.lib gdi32.lib user32.lib runtimeobject.lib || exit /b 1

echo [2/2] GodoiCan.exe
rc /nologo /fo obj\app\app.res app\app.rc || exit /b 1
cl %CFLAGS% app\*.cpp obj\app\app.res /Fo"obj\app\\" /Fe"bin\GodoiCan.exe" ^
   /link /SUBSYSTEM:WINDOWS mfplat.lib mfuuid.lib mf.lib mfsensorgroup.lib wmcodecdspuuid.lib strmiids.lib ^
   ws2_32.lib iphlpapi.lib bcrypt.lib crypt32.lib comctl32.lib shell32.lib ole32.lib oleaut32.lib ^
   user32.lib gdi32.lib advapi32.lib || exit /b 1

echo OK: %CD%\bin
