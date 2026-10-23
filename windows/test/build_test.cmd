@echo off
setlocal
cd /d "%~dp0"
"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\godoican_vs.txt"
set /p VS=<"%TEMP%\godoican_vs.txt"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist ..\obj\test mkdir ..\obj\test
cl /nologo /std:c++17 /EHsc /O2 /utf-8 /DUNICODE /D_UNICODE /I..\common decodetest.cpp ..\app\Decoder.cpp /Fo..\obj\test\ /Fe..\bin\decodetest.exe /link mfplat.lib mfuuid.lib wmcodecdspuuid.lib ole32.lib oleaut32.lib
