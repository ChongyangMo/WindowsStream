@echo off
setlocal
net session >nul 2>&1
if errorlevel 1 (
    echo Please right-click this file and choose "Run as administrator".
    pause
    exit /b 1
)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup-link.ps1" %*
if errorlevel 1 (
    echo Setup failed. Review the error above.
    pause
    exit /b 1
)
pause
exit /b 0
