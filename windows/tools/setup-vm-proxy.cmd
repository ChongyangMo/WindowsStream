@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup-vm-proxy.ps1" %*
if errorlevel 1 (
    pause
    exit /b 1
)
pause
exit /b 0
