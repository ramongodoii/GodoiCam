@echo off
rem Remove a camera virtual, o registro da DLL, o atalho e a pasta do GodoiCam.
setlocal
net session >nul 2>&1
if errorlevel 1 (
    powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
    exit /b
)

set "DEST=%ProgramFiles%\GodoiCam"
taskkill /im GodoiCam.exe /f >nul 2>&1
if exist "%DEST%\GodoiCam.exe" "%DEST%\GodoiCam.exe" --remove-camera
if exist "%DEST%\GodoiCamVCam.dll" regsvr32 /s /u "%DEST%\GodoiCamVCam.dll"
netsh advfirewall firewall delete rule name="GodoiCam" >nul 2>&1
del "%ProgramData%\Microsoft\Windows\Start Menu\Programs\GodoiCam.lnk" >nul 2>&1
rmdir /s /q "%DEST%" 2>nul
echo GodoiCam removido. Os pareamentos ficam em %APPDATA%\GodoiCam (apague se quiser).
pause
