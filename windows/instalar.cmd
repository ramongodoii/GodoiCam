@echo off
rem Instala o GodoiCan para Windows em "Arquivos de Programas" e registra a camera virtual.
rem Pede permissao de administrador uma unica vez.
setlocal
net session >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

set "SRC=%~dp0bin"
set "DEST=%ProgramFiles%\GodoiCan"
if not exist "%SRC%\GodoiCan.exe" (
    echo Compile primeiro com build.cmd.
    pause
    exit /b 1
)

taskkill /im GodoiCan.exe /f >nul 2>&1
if not exist "%DEST%" mkdir "%DEST%"
copy /y "%SRC%\GodoiCan.exe" "%DEST%\" >nul || goto :fail
copy /y "%SRC%\GodoiCanVCam.dll" "%DEST%\" >nul || goto :fail

rem O Frame Server do Windows carrega esta DLL; ela precisa estar registrada em HKLM.
regsvr32 /s "%DEST%\GodoiCanVCam.dll" || goto :fail

netsh advfirewall firewall delete rule name="GodoiCan" >nul 2>&1
netsh advfirewall firewall add rule name="GodoiCan" dir=in action=allow program="%DEST%\GodoiCan.exe" enable=yes profile=any >nul

powershell -NoProfile -Command "$s=(New-Object -ComObject WScript.Shell).CreateShortcut('%ProgramData%\Microsoft\Windows\Start Menu\Programs\GodoiCan.lnk'); $s.TargetPath='%DEST%\GodoiCan.exe'; $s.WorkingDirectory='%DEST%'; $s.Save()"

echo.
echo GodoiCan instalado em %DEST%
echo Abrindo o app (sem administrador)...
rem explorer.exe abre o app com o usuario normal, nao como administrador.
explorer.exe "%DEST%\GodoiCan.exe"
timeout /t 4 >nul
exit /b 0

:fail
echo Falha na instalacao.
pause
exit /b 1
