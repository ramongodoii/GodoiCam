@echo off
setlocal
cd /d "%~dp0"
"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\godoicam_vs.txt"
set /p VS=<"%TEMP%\godoicam_vs.txt"
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul
if not exist ..\obj\test mkdir ..\obj\test
cl /nologo /std:c++17 /EHsc /O2 /utf-8 /DUNICODE /D_UNICODE /I..\common decodetest.cpp ..\app\Decoder.cpp /Fo..\obj\test\ /Fe..\bin\decodetest.exe /link mfplat.lib mfuuid.lib wmcodecdspuuid.lib ole32.lib oleaut32.lib d3d11.lib dxgi.lib
cl /nologo /std:c++17 /EHsc /O2 /utf-8 /DUNICODE /D_UNICODE camprobe.cpp /Fo..\obj\test\ /Fe..\bin\camprobe.exe /link mfplat.lib mf.lib mfreadwrite.lib mfuuid.lib ole32.lib
cl /nologo /std:c++17 /EHsc /O2 /utf-8 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /DNOMINMAX atraso.cpp /Fo..\obj\test\ /Fe..\bin\atraso.exe /link mfplat.lib mf.lib mfreadwrite.lib mfuuid.lib ole32.lib user32.lib gdi32.lib shell32.lib
