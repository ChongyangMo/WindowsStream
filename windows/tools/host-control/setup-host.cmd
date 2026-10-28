@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup-host.ps1"
if errorlevel 1 (
    pause
    exit /b 1
)
pause
exit /b 0
