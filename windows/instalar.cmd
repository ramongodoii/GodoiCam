@echo off
rem Instala o GodoiCam para Windows em "Arquivos de Programas" e registra a camera virtual.
rem Pede permissao de administrador uma unica vez.
setlocal
net session >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

set "SRC=%~dp0bin"
set "DEST=%ProgramFiles%\GodoiCam"
if not exist "%SRC%\GodoiCam.exe" (
    echo Compile primeiro com build.cmd.
    pause
    exit /b 1
)

taskkill /im GodoiCam.exe /f >nul 2>&1
call :legado

if not exist "%DEST%" mkdir "%DEST%"
copy /y "%SRC%\GodoiCam.exe" "%DEST%\" >nul || goto :fail
rem A DLL fica aberta enquanto o Frame Server usa a camera: so troca se mudou.
fc /b "%SRC%\GodoiCamVCam.dll" "%DEST%\GodoiCamVCam.dll" >nul 2>&1 || (
    copy /y "%SRC%\GodoiCamVCam.dll" "%DEST%\" >nul || goto :fail_dll
)

rem O Frame Server do Windows carrega esta DLL; ela precisa estar registrada em HKLM.
regsvr32 /s "%DEST%\GodoiCamVCam.dll" || goto :fail

netsh advfirewall firewall delete rule name="GodoiCam" >nul 2>&1
netsh advfirewall firewall add rule name="GodoiCam" dir=in action=allow program="%DEST%\GodoiCam.exe" enable=yes profile=any >nul

powershell -NoProfile -Command "$s=(New-Object -ComObject WScript.Shell).CreateShortcut('%ProgramData%\Microsoft\Windows\Start Menu\Programs\GodoiCam.lnk'); $s.TargetPath='%DEST%\GodoiCam.exe'; $s.WorkingDirectory='%DEST%'; $s.Save()"

echo.
echo GodoiCam instalado em %DEST%
echo Abrindo o app (sem administrador)...
rem explorer.exe abre o app com o usuario normal, nao como administrador.
explorer.exe "%DEST%\GodoiCam.exe"
timeout /t 4 >nul
exit /b 0

:fail_dll
echo A camera virtual esta em uso (Frame Server do Windows). Feche os apps que usam a
echo camera GodoiCam ou reinicie o PC e rode o instalar.cmd de novo.

:fail
echo Falha na instalacao.
pause
exit /b 1

rem Remove a instalacao antiga com o nome "GodoiCan" e leva preferencias e dados para "GodoiCam".
rem Precisa rodar antes de registrar a DLL nova: as duas usam o mesmo CLSID.
:legado
set "OLD=%ProgramFiles%\GodoiCan"
set "RUN=HKCU\Software\Microsoft\Windows\CurrentVersion\Run"
taskkill /im GodoiCan.exe /f >nul 2>&1
if exist "%OLD%\GodoiCan.exe" "%OLD%\GodoiCan.exe" --remove-camera
if exist "%OLD%\GodoiCanVCam.dll" regsvr32 /s /u "%OLD%\GodoiCanVCam.dll"
netsh advfirewall firewall delete rule name="GodoiCan" >nul 2>&1
del "%ProgramData%\Microsoft\Windows\Start Menu\Programs\GodoiCan.lnk" >nul 2>&1
if exist "%OLD%" rmdir /s /q "%OLD%" 2>nul
reg query "%RUN%" /v GodoiCan >nul 2>&1 && (
    reg delete "%RUN%" /v GodoiCan /f >nul
    reg add "%RUN%" /v GodoiCam /t REG_SZ /d "\"%DEST%\GodoiCam.exe\" --minimized" /f >nul
)
reg query "HKCU\Software\GodoiCan" >nul 2>&1 && (
    reg copy "HKCU\Software\GodoiCan" "HKCU\Software\GodoiCam" /s /f >nul
    reg delete "HKCU\Software\GodoiCan" /f >nul
)
if exist "%APPDATA%\GodoiCan" if not exist "%APPDATA%\GodoiCam" move "%APPDATA%\GodoiCan" "%APPDATA%\GodoiCam" >nul
exit /b 0
