@echo off
rem Remove a camera virtual, o registro da DLL, o atalho e a pasta do GodoiCan.
setlocal
net session >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

set "DEST=%ProgramFiles%\GodoiCan"
taskkill /im GodoiCan.exe /f >nul 2>&1
if exist "%DEST%\GodoiCan.exe" "%DEST%\GodoiCan.exe" --remove-camera
if exist "%DEST%\GodoiCanVCam.dll" regsvr32 /s /u "%DEST%\GodoiCanVCam.dll"
netsh advfirewall firewall delete rule name="GodoiCan" >nul 2>&1
del "%ProgramData%\Microsoft\Windows\Start Menu\Programs\GodoiCan.lnk" >nul 2>&1
rmdir /s /q "%DEST%" 2>nul
echo GodoiCan removido. Os pareamentos ficam em %APPDATA%\GodoiCan (apague se quiser).
pause
