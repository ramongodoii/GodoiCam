@echo off
rem Compila o app (se preciso) e gera windows\bin\GodoiCam-Setup.exe com o Inno Setup.
setlocal
cd /d "%~dp0"

if not exist "bin\GodoiCam.exe" (
    echo Compilando o GodoiCam...
    call build.cmd || exit /b 1
)
if not exist "bin\GodoiCamVCam.dll" (
    echo Compilando o GodoiCam...
    call build.cmd || exit /b 1
)

set "ISCC="
if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not defined ISCC (
    echo Inno Setup 6 nao encontrado. Instale com:
    echo   winget install JRSoftware.InnoSetup
    exit /b 1
)

"%ISCC%" /Q godoicam.iss || exit /b 1
echo OK: %CD%\bin\GodoiCam-Setup.exe
